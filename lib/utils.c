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

#include <utils.h>

void _sizebuf(struct buffer *buf, size_t wanted, size_t el)
{
    size_t n;
    
    n = buf->s;
    if(n == 0)
	n = 1;
    while(n < wanted)
	n <<= 1;
    if(n <= buf->s)
	return;
    if(buf->b != NULL)
	buf->b = srealloc(buf->b, n * el);
    else
	buf->b = smalloc(n * el);
    buf->s = n;
}

