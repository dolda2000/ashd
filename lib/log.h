#ifndef _LOG_H
#define _LOG_H

#include <syslog.h>

void flog(int level, char *format, ...);
void opensyslog(void);

#endif
