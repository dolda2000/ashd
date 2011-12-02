#!/usr/bin/python3

from distutils.core import setup, Extension

htlib = Extension("ashd.htlib", ["htp.c"],
                  libraries = ["ht"])

setup(name = "ashd-py3",
      version = "0.4",
      description = "Python module for handling ashd requests",
      author = "Fredrik Tolf",
      author_email = "fredrik@dolda2000.com",
      url = "http://www.dolda2000.com/~fredrik/ashd/",
      ext_modules = [htlib],
      packages = ["ashd"],
      scripts = ["ashd-wsgi3", "scgi-wsgi3"],
      license = "GPL-3")
