#include "passwordConfirm.h"
#include <shadow.h>
#include <crypt.h>
#include <iostream>
#include <string>
#include <stdio.h>
#include <string.h>
 

/*
 * @brief 检查用户名和密码是否匹配
 * @param in userName 用户名
 * @param in password 密码
 * @return 当正确匹配时返回0，当密码错误或者用户名不存在时返回-1
 */
int passwordConfirm(const char *userName, const char *password){
    struct spwd *sp;
    sp = getspnam(userName);  //函数返回结构体指针 结构体中存储用户信息 返回空指针代表用户不存在
    if(sp == nullptr){
        std::cout << "Error:user name doesn't exist!" << std::endl;
        return -1;
    }
    char salt[20] = {0};
    getSalt(sp->sp_pwdp, salt);

    //把用户密码与盐值加密后得到新的密文   再与系统密文比较
    char *pNewPassword = crypt(password, salt);
    if(strcmp(sp->sp_pwdp, pNewPassword) == 0){
        std::cout << "Match successful!" << std::endl;
        return 0;
    }
    else{
        std::cout << "Wrong password!" << std::endl;
        return -1;
    }
}

/*
 *@brief 从系统存储的加密密码中获取用户盐值
 *@param in systemPassword /etc/shadow 目录下存储的加密的用户密码
 *@param out salt 截取的用户密码对应的盐值
 */
static void getSalt(const char *systemPassword, char *salt){
    int i,j;
    
    //获取盐值长度,盐值以$开头，至第3个$前结束（不包括第3个$）
    for(int i = 0, j = 0 ; systemPassword[i] != '\0' && j != 3 ; ++i){
        if(systemPassword[i] == '$')
            ++j;
    }
    strncpy(salt, systemPassword, i - 1);
}