#!/usr/bin/python

from distutils.core import setup, Extension

htlib = Extension("ashd.htlib", ["htp.c"],
                  libraries = ["ht"])

setup(name = "ashd-py",
      version = "0.6",
      description = "Python module for handling ashd requests",
      author = "Fredrik Tolf",
      author_email = "fredrik@dolda2000.com",
      url = "http://www.dolda2000.com/~fredrik/ashd/",
      ext_modules = [htlib],
      packages = ["ashd"],
      scripts = ["ashd-wsgi", "scgi-wsgi", "htredir"],
      license = "GPL-3")
