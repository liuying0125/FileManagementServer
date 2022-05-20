#include "http_conn.h"
#include "../passwordConfirm/passwordConfirm.h"
#include "../passwordConfirm/md5.h"
#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
        //先从连接池中取一个连接
        MYSQL *mysql = NULL;
        connectionRAII mysqlcon(&mysql, connPool);

        //在user表中检索username，passwd数据，浏览器端输入
        if (mysql_query(mysql, "SELECT username,passwd FROM user"))
        {
                LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        }

        //从表中检索完整的结果集
        MYSQL_RES *result = mysql_store_result(mysql);

        //返回结果集中的列数
        int num_fields = mysql_num_fields(result);

        //返回所有字段结构的数组
        MYSQL_FIELD *fields = mysql_fetch_fields(result);

        //从结果集中获取下一行，将对应的用户名和密码，存入map中
        while (MYSQL_ROW row = mysql_fetch_row(result))
        {
                string temp1(row[0]);
                string temp2(row[1]);
                users[temp1] = temp2;
        }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
        int old_option = fcntl(fd, F_GETFL);
        int new_option = old_option | O_NONBLOCK;
        fcntl(fd, F_SETFL, new_option);
        return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
        epoll_event event;
        event.data.fd = fd;  
        if (1 == TRIGMode)
                event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        else
                event.events = EPOLLIN | EPOLLRDHUP;

        if (one_shot)
                event.events |= EPOLLONESHOT;
        epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
        setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
        close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)  //默认为 0  listenfd LT + connfd LT
{
        epoll_event event;
        event.data.fd = fd;

        if (1 == TRIGMode)
                event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
        else
                event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
        m_sockfd = sockfd;
        m_address = addr;

        addfd(m_epollfd, sockfd, true, m_TRIGMode);
        m_user_count++;

        //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
        doc_root = root;
        m_TRIGMode = TRIGMode;
        m_close_log = close_log;

        strcpy(sql_user, user.c_str());
        strcpy(sql_passwd, passwd.c_str());
        strcpy(sql_name, sqlname.c_str());

        init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
        mysql = NULL;
        bytes_to_send = 0;
        bytes_have_send = 0;
        m_check_state = CHECK_STATE_REQUESTLINE;
        m_linger = false;
        m_method = GET;
        m_url = 0;
        m_version = 0;
        m_content_length = 0;
        m_host = 0;
        m_start_line = 0;
        m_checked_idx = 0;
        m_read_idx = 0;
        m_write_idx = 0;
        cgi = 0;
        m_state = 0;
        timer_flag = 0;
        improv = 0;

        memset(m_read_buf, '\0', READ_BUFFER_SIZE);
        memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
        memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
// 从状态机读取一行，分析是请求报文的哪一部分  +
http_conn::LINE_STATUS http_conn::parse_line()
{
       // std::cout << "parse_line()" << std::endl;
      
        char temp;
     //   std::cout << "m_checked_idx = :" << m_checked_idx << std::endl;
      //  std::cout << "m_read_idx = :" << m_read_idx << std::endl;

        for (; m_checked_idx < m_read_idx; ++m_checked_idx)
        {        //  temp为将要分析的字节
                temp = m_read_buf[m_checked_idx];
                //  如果当前是\r字符，则有可能会读取到完整行 +
                if (temp == '\r')
                {
                        //下一个字符达到了buffer结尾，则接收不完整，需要继续接收   +
                        if ((m_checked_idx + 1) == m_read_idx)
                                return LINE_OPEN;
                        
                        //下一个字符是\n，将\r\n改为\0\0     +
                        else if (m_read_buf[m_checked_idx + 1] == '\n')
                        {
                                m_read_buf[m_checked_idx++] = '\0';
                                m_read_buf[m_checked_idx++] = '\0';
                                return LINE_OK;
                        }
                        //如果都不符合，则返回语法错误  +
                        return LINE_BAD;
                }


                //如果当前字符是\n，也有可能读取到完整行  +
                //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
                else if (temp == '\n')
                {
                        if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
                        {
                                //前一个字符是\r，则接收完整
                                m_read_buf[m_checked_idx - 1] = '\0';
                                m_read_buf[m_checked_idx++] = '\0';
                                return LINE_OK;
                        }
                        return LINE_BAD;
                }
        }
        //并没有找到\r\n，需要继续接收  
        //这时候  m_checked_idx++
        return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接 + 
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
        if (m_read_idx >= READ_BUFFER_SIZE)
        {
                return false;
        }
        int bytes_read = 0;

        //LT读取数据
        if (0 == m_TRIGMode)
        {
                bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
                m_read_idx += bytes_read;

                if (bytes_read <= 0){
                        return false;
                } 
                return true;
        }
        //ET读数据
        else
        {
                while (true)
                {
                        //  从套接字接收数据，存储在m_read_buf缓冲区 +
                        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
                        if (bytes_read == -1){
                                 //非阻塞ET模式下，需要一次性将数据读完 +
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                        break;
                                return false;
                        }
                        else if (bytes_read == 0){
                                return false;
                        }
                        // 修改m_read_idx的读取字节数 +
                        m_read_idx += bytes_read;
                }
                return true;
        }
}

//解析http请求行        获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
        std::cout << "请求行 :" << text << endl;
        //printf("%s\n",text);

        //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
        //请求行中最先含有空格和\t任一字符的位置并返回

        m_url = strpbrk(text, " \t");  //在源字符串（s1）中找出最先含有搜索字符串（s2）中任一字符的位置并返回  
        std::cout << "text :" << text << endl;
        // text 是以 \t\n 结尾的
        if (!m_url)
        {
                return BAD_REQUEST;
        }
        *m_url++ = '\0';
        char *method = text;
        std::cout << "method  =  " << method << std::endl;
        if (strcasecmp(method, "GET") == 0)
                m_method = GET;
        else if (strcasecmp(method, "POST") == 0)
        {
                m_method = POST;
                cgi = 1;
        }
        else
                return BAD_REQUEST;
                                            //        ABCDEFGHI                ABCD
        m_url += strspn(m_url, " \t");      //检索字符串 m_url 中第一个不在字符串 " \t" 中出现的字符下标。
        m_version = strpbrk(m_url, " \t");
        if (!m_version)
                return BAD_REQUEST;
        *m_version++ = '\0';
        m_version += strspn(m_version, " \t");  
        if (strcasecmp(m_version, "HTTP/1.1") != 0)
                return BAD_REQUEST;
        if (strncasecmp(m_url, "http://", 7) == 0)  //m_url 和  http:// 的前7个字符
        {
                m_url += 7;
                m_url = strchr(m_url, '/');  //字符串m_url中寻找字符  '/' 第一次出现的位置。
        }

        if (strncasecmp(m_url, "https://", 8) == 0)   //http 与 https
        {
                m_url += 8;
                m_url = strchr(m_url, '/');
        }

        if (!m_url || m_url[0] != '/')
                return BAD_REQUEST;
        //当url为/时，显示判断界面
        if (strlen(m_url) == 1)  //  Request URL: http://43.138.12.215:9006/1
                strcat(m_url, "judge.html");
        m_check_state = CHECK_STATE_HEADER;
        return NO_REQUEST;
}

// 解析http请求的一个头部信息  
// 解析请求头部，GET和POST中      空行以上，请求行以下     的部分 +
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
        cout << "头部信息 :" << text << endl;
        //判断是空行还是请求头
        if (text[0] == '\0')
        {        //判断是GET还是POST请求
                if (m_content_length != 0)
                {       //POST需要跳转到消息体处理状态
                        m_check_state = CHECK_STATE_CONTENT;
                        return NO_REQUEST;
                }
                return GET_REQUEST;
        }
        //解析请求头部连接字段  + 
        else if (strncasecmp(text, "Connection:", 11) == 0)
        {
                text += 11;
                //跳过空格和\t字符  
                text += strspn(text, " \t");
                if (strcasecmp(text, "keep-alive") == 0)
                {
                        //如果是长连接，则将linger标志设置为true +
                        m_linger = true;
                }
        }
        //解析请求头部内容长度字段  +
        else if (strncasecmp(text, "Content-length:", 15) == 0)
        {
                text += 15;
                text += strspn(text, " \t");
                m_content_length = atol(text);
        }
        //解析请求头部HOST字段  +
        else if (strncasecmp(text, "Host:", 5) == 0)
        {
                text += 5;
                text += strspn(text, " \t");
                m_host = text;
        }
        else
        {
                LOG_INFO("oop!unknow header: %s", text);
        }
        return NO_REQUEST;
}

//判断http请求是否被完整读入
/*
    解析请求数据，对于GET来说这部分是空的，因为这部分内容已经以明文的方式包含在了请求行中的URL部分了 +
    只有POST的这部分是有数据的，项目中的这部分数据为用户名和密码， +
    我们会根据这部分内容做登录和校验，并涉及到与数据库的连接。  +
*/
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
        std::cout << "parse_content()" << endl;
        ////判断buffer中是否读取了消息体 +
        if (m_read_idx >= (m_content_length + m_checked_idx))
        {
            text[m_content_length] = '\0';
            
            //POST请求中最后为输入的用户名和密码
            m_string = text;
            cout << "m_string = " << m_string << endl;  // +
            return GET_REQUEST;
        }
        return NO_REQUEST;
}

// 该connfd读缓冲区的请求报文进行解析 +
http_conn::HTTP_CODE http_conn::process_read()
{
        //初始化从状态机状态、HTTP请求解析结果 +
        LINE_STATUS line_status = LINE_OK;
        HTTP_CODE ret = NO_REQUEST;
        char *text = 0;

        // http类init的时候 m_check_state = CHECK_STATE_REQUESTLINE;   所以一开始一定会执行 parse_line  
        // 于是 m_checked_idx 变成了 27/28
        while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
        { 
                text = get_line(); 
                std::cout << "text = get_line() = :" << text << std::endl; 
                //                  text = get_line();  =  POST /2CGISQL.cgi HTTP/1.1\r\n   正好28个字符
                m_start_line = m_checked_idx;
                std::cout << "m_start_line = :" << m_start_line << std::endl; 
                LOG_INFO("%s", text);

                //主状态机的三种状态转移逻辑  +
                switch (m_check_state)
                {
                        case CHECK_STATE_REQUESTLINE:
                        {
                            std::cout << "CHECK_STATE_REQUESTLINE" << std::endl;
                                ret = parse_request_line(text);  //解析请求行

                                if (ret == BAD_REQUEST)   //HTTP请求报文有语法错误
                                        return BAD_REQUEST;
                                break;
                        }

                        case CHECK_STATE_HEADER:
                        {
                            std::cout << "CHECK_STATE_HEADER" << std::endl;
                                ret = parse_headers(text);   //解析请求头

                                if (ret == BAD_REQUEST)    //HTTP请求报文有语法错误
                                        return BAD_REQUEST;  

                                else if (ret == GET_REQUEST)  //获得了完整的HTTP请求
                                {        //完整解析GET请求后，跳转到报文响应函数
                                        return do_request();
                                }
                                break;
                        }

                        case CHECK_STATE_CONTENT:
                        {
                            std::cout << "CHECK_STATE_CONTENT" << std::endl;
                                ret = parse_content(text);   //解析消息体

                                //解析完消息体即完成报文解析，避免再次进入循环，更新line_status  + 
                                if (ret == GET_REQUEST)   
                                        return do_request();
                                line_status = LINE_OPEN;
                                break;
                        }
                        default:
                                return INTERNAL_ERROR;
                }
        }
        return NO_REQUEST;
}



/*我们需要首先对 GET请求和不同 POST请求（登录，注册，请求图片，视频等等）做不同的预处理，
然后分析目标文件的属性，若目标文件存在、对所有用户可读且不是目录时，
则使用 mmap将其映射到内存地址 m_file_address处，并告诉调用者获取文件成功。 +++
*/
// 生成响应报文 +
http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录 +
    strcpy(m_real_file, doc_root);    //  m_real_file = :/home/ubuntu/TinyWebServer-master/root
    char* downloadDir = m_real_file + '/DownLoadDir';
    int len = strlen(doc_root);
    printf("do_request起始的m_url:%s\n", m_url);

    
    //找到m_url中/的位置 + 
    const char *p = strrchr(m_url, '/');  //strrchr  在参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置。

    //处理cgi   //实现登录和注册校验 + 
    std::cout << *(p+1) << std::endl;
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }


    if (*(p + 1) == 'u'){
        std::cout << "上传" << std::endl;
    }

    //如果请求资源为/0，表示跳转注册界面 +
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中 +
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

        //如果请求资源为/1，表示跳转登录界面 +
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中 +
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //下载功能-
    else if (*(p + 1) == '8')
    {
        // 写一个 读取 Downloadroot文件夹下的文件的 新的 dowmload.html
        makeNewDownloadHTML("./root/Downloadroot");

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/download.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //上传功能-
     else if (*(p + 1) == '9')
    { 
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/upload2.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //----------添加代码----
    else 
        //std::cout << "strncpy()函数之前m_real_file = :" << m_real_file << endl;   ???? 
        //strncpy()函数之前m_real_file = :/home/ubuntu/TinyWebServer-master/root   len = 38
        //std::cout << "len = " << len << std::endl; ???? 加上没有办法显示judge界面
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);  // m_url 复制到 m_real_file + len  
      
        printf("m_url:%s\n", m_url);
        //通过stat获取请求资源文件信息，成功则将信息更新到 m_file_stat 结构体 +
        //失败返回NO_RESOURCE状态，表示资源不存在 +
        std::cout << "else 中 m_real_file = " << m_real_file << std::endl;
        //            else 中 m_real_file = /home/ubuntu/TinyWebServer-master/root/judge.html
        

        if (stat(m_real_file, &m_file_stat) < 0)   // m_real_file 是路径名字   m_file_stat 是文件的状态
            return NO_RESOURCE;

        //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态 +
        if (!(m_file_stat.st_mode & S_IROTH))
            return FORBIDDEN_REQUEST;

        //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误 +
        if (S_ISDIR(m_file_stat.st_mode))
            return BAD_REQUEST;

        std::cout << "m_real_file = :" << m_real_file << std::endl;
        std::cout << "open对应的 m_real_file 文件到 fd 中" << std::endl;
        int fd = open(m_real_file, O_RDONLY);
        //使用mmap将其映射到内存地址 m_file_address 处，并告诉调用者获取文件成功 +
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        return FILE_REQUEST;
}


//处理 download HTML 文件
void http_conn::makeNewDownloadHTML(string strDir){
    //string Dir =  "./Downloadroot";
    vector<string> vFileFullPath;
    std::cout << "进入处理HTML函数  strDir = " << strDir << std::endl;
    int fd = -1;
    int ret = -1;
   // char writeBuf[10] = "#####";
    vector<string> s1;

    struct dirent* pDirent;
    DIR* pDir = opendir(strDir.c_str());
    if (pDir != NULL)
    {
            while ((pDirent = readdir(pDir)) != NULL)
            {
                    string strFileName = pDirent->d_name;
                    string strFileFullPath = strDir + "/" + strFileName;
                    if(strFileName != "." && strFileName != ".."){
                        vFileFullPath.push_back(strFileFullPath);
                    }
            }
            // vFileFullPath.erase(vFileFullPath.begin(), vFileFullPath.begin() +  2);    //前两个存储的是当前路径和上一级路径，所以要删除
    }else
        std::cout << "strDir文件不存在" << std::endl;

    for(auto c : vFileFullPath){
        std::cout << c << std::endl;
    }
    std::cout << "处理HTML函数 over   "   << std::endl;

    char* downloadexample = "./root/downloadexample.txt";   //  HTML的 示例文件 修改都在这里
    char* downloadhtml = "./root/download.html";

    int fdexample = open(downloadexample,O_RDWR,0666);   //打开示例HTML文件
    if(fdexample == -1){
        perror("打开文件失败downloadexample.txt");
        _exit(-1);
    }

    //获取示例HTML文件的大小
    struct stat statbuf;
    stat(downloadexample,&statbuf);
    int sizeHTMLexample = statbuf.st_size;

     

    fd = open(downloadhtml,O_RDWR,0666);  //打开目标 HTML
    if(fd == -1){   
        perror("打开文件失败");
        _exit(-1);
    }
     //清空html的内容  或者直接覆盖之前的内容  感觉还是清空比较 稳定
    fstream file;
    fstream open(downloadhtml,ios::out|ios::binary);

    char HTMLexample[sizeHTMLexample];
    read(fdexample,HTMLexample,sizeof(HTMLexample));  //读取示例文件到 HTMLexample字符
    write(fd,HTMLexample,sizeof(HTMLexample));   //写入示例文件

    //设置偏移
    struct stat info; 
    stat(downloadhtml, &info); 
    int size = info.st_size; 
  

    ret = lseek(fd,size,SEEK_SET);
    for(int i = 0 ; i < vFileFullPath.size(); i++){
        char br[] = "\<br\/\> \<a\ href\=\"";
        char br1[] = "\"\ download\=\"";
        char br2[] = "\"\>";
        char br3[] = "\<\/a\>\n";
        string vF ="." + vFileFullPath[i].substr(6);
        string down = vFileFullPath[i].substr(20);
        char *downchar = (char*)down.c_str();
        char *p = (char*)vF.c_str();
        write(fd, br, strlen(br));
        write(fd, p, strlen(p));
        write(fd, br1, strlen(br1));
        write(fd, downchar, strlen(downchar));
        write(fd, br2, strlen(br2));
        write(fd, downchar, strlen(downchar));
        write(fd, br3, strlen(br3));

    }
    char br4[] = "\<\/html\>\n";
    write(fd, br4, strlen(br4));
    // int n = write(fd,writeBuf,strlen(writeBuf));
    // printf("写入了%d个字节\n",n);
    // printf("偏移了%d个字节\n",ret);

    close(fd);

}


void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//将响应报文发送给浏览器端口 +
bool http_conn::writetocom()
{
    std::cout << "将响应报文发送给浏览器端口" << std::endl;
    int temp = 0;
    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端 +
        temp = writev(m_sockfd, m_iv, m_iv_count);   //若成功则返回已读、写的字节数 + 

        //正常发送，temp为发送的字节数 +
        if (temp < 0)
        {
            //判断缓冲区是否满了 +
            if (errno == EAGAIN)
            {
                    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); //将该m_epollfd文件描述符上修改为EPOLLOUT（可写）事件 +
                    return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射 + 
            unmap();
            return false;
        }
        //更新已发送字节  +
        bytes_have_send += temp;
        // std::cout << "bytes_have_send = " << bytes_have_send << std::endl;
       
       
        //更新还需要发送字节数
        bytes_to_send -= temp;

        // std::cout << "bytes_to_send = " << bytes_to_send << endl; 
        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据 +
        if (bytes_have_send >= m_iv[0].iov_len)  //不再继续发送头部信息 +
        {
            m_iv[0].iov_len = 0;   //???为什么要设置为 0     
            std::cout << "----------------"<< std::endl;
            std::cout << "m_file_address = :" << m_file_address << std::endl;  // 装载的是 html 
            std::cout << "bytes_have_send = :" << bytes_have_send << std::endl;
            std::cout << "m_write_idx = :" << m_write_idx << std::endl;  //状态行 + 响应头部     
            std::cout << "----------------" << std::endl;
            //      下面发送的就是响应行之后的数据
            std::cout << "(bytes_have_send - m_write_idx) = " << (bytes_have_send - m_write_idx) << std::endl; 
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);  //明白了这是已经writev之后的
            // 因为每一次发送都要加上头 所以 在m_iv[1]中损失的实际上是  (bytes_have_send-响应头(也就是m_write_idx个数据))
            std::cout << "bytes_to_send = " << bytes_to_send << std::endl;    //随着发送减少
            m_iv[1].iov_len = bytes_to_send;
        }
        else  //继续发送第一个iovec头部信息的数据 + 
        {
            //应该是第一个 writev已经发送完毕了
            std::cout << "m_iv[0].iov_len - bytes_have_send = " << m_iv[0].iov_len - bytes_have_send << std::endl;
            m_iv[0].iov_base = m_write_buf    + bytes_have_send;    //  响应行  HTTP ??? 
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //判断条件，数据已全部发送完 +
        if (bytes_to_send <= 0)
        {
            unmap();
            //在epoll树上重置EPOLLONESHOT事件 +
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //浏览器的请求为长连接 +
            if (m_linger)
            {
                //重新初始化HTTP对象 +
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

/*
add_status_line 函数，添加状态行：http/1.1 状态码 状态消息

add_headers 函数添加消息报头，内部调用add_content_length和add_linger函数

content-length 记录响应报文长度，用于浏览器端判断服务器是否发送完数据

connection 记录连接状态，用于告诉浏览器端保持长连接

add_blank_line 添加空行
*/
//调用add_response函数更新 m_write_idx 指针和缓冲区 m_write_buf 中的内容 
bool http_conn::add_response(const char *format, ...)
{
        //如果写入内容超出m_write_buf大小则报错 +
        if (m_write_idx >= WRITE_BUFFER_SIZE)
                return false;
        
        //定义可变参数列表 +
        va_list arg_list;

        //将变量arg_list初始化为传入参数 +
        va_start(arg_list, format);

        //将数据 format 从可变参数列表写入缓冲区写，返回写入数据的长度 +
        int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE-1-m_write_idx, format, arg_list);
        //                                                      1024
        //如果写入的数据长度超过缓冲区剩余空间，则报错
        if (len >= (WRITE_BUFFER_SIZE-1-m_write_idx))
        {
                va_end(arg_list);
                return false;
        }
        //更新m_write_idx位置 +
        m_write_idx += len;
        LOG_INFO("m_write_idx:%d",m_write_idx);
        //清空可变参列表 + 
        va_end(arg_list);

        LOG_INFO("request:%s  %d", m_write_buf,len);  // 加上 /r/n之后就是 17 

        return true;
}

//添加状态行 +
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加消息报头，具体的添加文本长度、连接状态和空行 +
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

//添加Content-Length，表示响应报文的长度 +
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

//添加文本类型，这里是html +
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭 +
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//添加空行 + 
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//添加文本content +
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//  该函数根据process_read()的返回结果来判断应该返回给用户什么响应 +
bool http_conn::process_write(HTTP_CODE ret)
{ 
        switch (ret)
        {
                //内部错误，500
                case INTERNAL_ERROR:
                {
                        //状态行
                        add_status_line(500, error_500_title);
                        //消息报头
                        add_headers(strlen(error_500_form));
                        if (!add_content(error_500_form))
                                return false;
                        break;
                }
                //报文语法有误，404
                case BAD_REQUEST:
                {
                        add_status_line(404, error_404_title);
                        add_headers(strlen(error_404_form));
                        if (!add_content(error_404_form))
                                return false;
                        break;
                }
                //资源没有访问权限，403
                case FORBIDDEN_REQUEST:
                {
                        add_status_line(403, error_403_title);
                        add_headers(strlen(error_403_form));
                        if (!add_content(error_403_form))
                                return false;
                        break;
                }
                //文件存在，200
                case FILE_REQUEST:
                {
                        std::cout << "装载响应行" << std::endl;  //发送数据时候不运行
                        add_status_line(200, ok_200_title);   //首先将状态行写入写缓存+
                                        // HTTP/1.1 200 OK
                        //如果请求的资源存在 +
                        if (m_file_stat.st_size != 0)
                        {
                                add_headers(m_file_stat.st_size);   // m_file_stat.st_size = 604

                                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                                m_iv[0].iov_base = m_write_buf;
                                m_iv[0].iov_len = m_write_idx;

                                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小 
                                m_iv[1].iov_base = m_file_address;
                                m_iv[1].iov_len = m_file_stat.st_size;
                                m_iv_count = 2;

                                //发送的全部数据为响应报文头部信息和文件大小 +
                                bytes_to_send = m_write_idx + m_file_stat.st_size;
                                return true;
                        }
                        else
                        {
                                const char *ok_string = "<html><body></body></html>";
                                add_headers(strlen(ok_string));
                                if (!add_content(ok_string))
                                        return false;
                        }
                }
                default:
                        return false;
        }

        //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区 +
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        m_iv_count = 1;
        bytes_to_send = m_write_idx;
        return true;
}

//处理HTTP请求的入口函数  + 
void http_conn::process()
{
        HTTP_CODE read_ret = process_read();   //对我们读入该connfd读缓冲区的请求报文进行解析 +
        if (read_ret == NO_REQUEST)
        {
                modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
                return;
        }
        bool write_ret = process_write(read_ret);
        if (!write_ret)
        {
                close_conn();
        }
        std::cout << "m_epollfd = " << m_epollfd << std::endl;
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
        // 当文件描述符中的缓冲区被读完了， 可以写了， 
        // 现在改为EPOLLOUT 监听可写，在主循环中被监听到，返回写事件
        // LT 模式下 只要可写，就一直往里写
}
