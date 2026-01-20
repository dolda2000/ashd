#ifndef SENDFILE_COMPRESS_H
#define SENDFILE_COMPRESS_H

int ccopen(char *path, struct stat *sb, char *accept, const char **encoding);

#endif
