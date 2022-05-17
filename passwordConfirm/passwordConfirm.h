#ifndef __PASSWORD_CONFIRM_H__
#define __PASSWORD_CONFIRM_H__

int passwordConfirm(const char *userName, const char *password);
static void getSalt(const char *systemPassword, char *salt);
#endif