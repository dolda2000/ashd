#!/usr/bin/python

from distutils.core import setup, Extension

htlib = Extension("ashd.htlib", ["htp.c"],
                  libraries = ["ht"],
                  library_dirs = ["../lib/"],
                  include_dirs = ["../lib/"])

setup(name = "ashd-py",
      version = "0.1",
      description = "Python module for handling ashd requests",
      author = "Fredrik Tolf",
      author_email = "fredrik@dolda2000.com",
      url = "http://www.dolda2000.com/~fredrik/ashd/",
      ext_modules = [htlib],
      packages = ["ashd"],
      license = "GPL-3")