/*
    ashd - A Sane HTTP Daemon
    Copyright (C) 2008  Fredrik Tolf <fredrik@dolda2000.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#include <utils.h>

static int tostderr = 1, tosyslog = 0;

void flog(int level, char *format, ...)
{
    va_list args;
    
    va_start(args, format);
    if(tostderr) {
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
    } else if(tosyslog) {
	vsyslog(level, format, args);
    }
    va_end(args);
}

void opensyslog(void)
{
    openlog("ashd", 0, LOG_DAEMON);
    tostderr = 0;
    tosyslog = 1;
}
