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

#include <Python.h>
#include <errno.h>

#include <utils.h>
#include <proc.h>

static PyObject *p_recvfd(PyObject *self, PyObject *args)
{
    int fd, ret;
    char *data;
    size_t dlen;
    PyObject *ro;
    
    fd = 0;
    if(!PyArg_ParseTuple(args, "|i", &fd))
	return(NULL);
    Py_BEGIN_ALLOW_THREADS;
    ret = recvfd(fd, &data, &dlen);
    Py_END_ALLOW_THREADS;
    if(ret < 0) {
	if(errno == 0)
	    return(Py_BuildValue("OO", Py_None, Py_None));
	PyErr_SetFromErrno(PyExc_OSError);
	return(NULL);
    }
    ro = Py_BuildValue("Ni", PyString_FromStringAndSize(data, dlen), ret);
    free(data);
    return(ro);
}

static PyObject *p_sendfd(PyObject *self, PyObject *args)
{
    int sock, fd, ret;
    PyObject *data;
    
    if(!PyArg_ParseTuple(args, "iiO", &sock, &fd, &data))
	return(NULL);
    if(!PyString_Check(data)) {
	PyErr_SetString(PyExc_TypeError, "datagram must be a string");
	return(NULL);
    }
    Py_BEGIN_ALLOW_THREADS;
    ret = sendfd(sock, fd, PyString_AsString(data), PyString_Size(data));
    Py_END_ALLOW_THREADS;
    if(ret < 0) {
	PyErr_SetFromErrno(PyExc_OSError);
	return(NULL);
    }
    Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"recvfd", p_recvfd, METH_VARARGS, "Receive a datagram and a file descriptor"},
    {"sendfd", p_sendfd, METH_VARARGS, "Send a datagram and a file descriptor"},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC inithtlib(void)
{
    Py_InitModule("ashd.htlib", methods);
}
