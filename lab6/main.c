#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#define SLEEP_NANOSECONDS 500000000
#define SLEEP_SECONDS 0
#define BUF_SIZE 4096

struct timespec sleep_time = {0, SLEEP_NANOSECONDS};

void create_path(char* newpath, const char* path, const char* filename) {
    strcpy(newpath, path);
    strcat(newpath, "/");
    strcat(newpath, filename);
}

char* get_name(char* abs_path) { 
    char* name = strrchr(abs_path, '/');
    name++;
    return name;
}

void free_paths(void* arg) {
    free(((char**)arg)[0]);
    free(((char**)arg)[1]);
    free((char*)arg);
}

void close_dir(void* arg) {
    if (closedir((DIR*)arg) != 0) {
        perror("Error closing dir");
    }
}
void close_file(void* arg) {
    if (close(*(int*)arg) != 0) {
        perror("Error closing file");
    }
}

void* copy_regfile(void* arg) {
    char* src_path = ((char**)arg)[0];
    char* dst_path = ((char**)arg)[1];
    pthread_cleanup_push(free_paths, arg);
    int src_flags = O_RDONLY;
    int src_fd = open(src_path, src_flags);
    while (src_fd == -1 && errno == EMFILE) {//Waiting for a file descriptor
        nanosleep(&sleep_time, NULL);
        src_fd = open(src_path, src_flags);
    }
    if (src_fd == -1) {
        perror(src_path);
        pthread_exit(0);
    }
    pthread_cleanup_push(close_file, &src_fd);

    char* src_name = get_name(src_path);
    char* dst_newpath = malloc(strlen(dst_path) + strlen(src_name) + 2);
    pthread_cleanup_push(free, dst_newpath);
    create_path(dst_newpath, dst_path, src_name); 
    struct stat statbuf;
    if (stat(dst_newpath, &statbuf) == 0 && (statbuf.st_mode & S_IFMT) != S_IFREG) { //If file with a same name already exists, check if it is regular. If not print error, else replace it.
        fputs("Cannot replace file ", stderr);
        fputs(dst_newpath, stderr);
        fputc('\n', stderr);
        pthread_exit(0);
    }
    stat(src_path, &statbuf);
    int dst_fd = creat(dst_newpath, statbuf.st_mode);
    while (dst_fd == -1 && errno == EMFILE) { //Waiting for a file descriptor
       nanosleep(&sleep_time, NULL);
       dst_fd = creat(dst_path, statbuf.st_mode);
    } 
    if (dst_fd == -1) {
        perror(dst_path);
        pthread_exit(0);
    }
    pthread_cleanup_push(close_file, &dst_fd);

    char buf[BUF_SIZE];
    ssize_t read_ret;
    ssize_t write_ret;
    int pos;
    int write_size;
    while ((read_ret = read(src_fd, buf, BUF_SIZE)) != 0) {
        if (read_ret == -1 && errno == EINTR) {//If reading is interrupted by signal, repeat reading
            continue;
        } else if (read_ret == -1) {
            perror(src_path);
            pthread_exit(0);
        }
        pos = 0;
        while ((write_ret = write(dst_fd, buf + pos, read_ret)) < read_ret) { //write until all bytes are written
            if (write_ret == -1 && errno == EINTR) {//If interrupted, repeat
                continue;
            } else if (write_ret == -1) {
                perror(dst_path);
                pthread_exit(0);
            }
            read_ret -= write_ret;
            pos += write_ret;
        } 
    }
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    pthread_exit(0);
}

void destroy_attr(void* arg) {
    if (pthread_attr_destroy(arg) != 0) {
        perror("Error destroying pthread attributes");
    }
}


void* copy_dir(void* arg) {
    char* src_path = ((char**)arg)[0];
    char* dst_path = ((char**)arg)[1];
    pthread_cleanup_push(free_paths, arg);
    if (strstr(dst_path, src_path) == dst_path) { //checking if dst_path has src_path in it
        fputs("Cannot copy into same directory or subdirectory\n", stderr);
        pthread_exit(0);
    }
    DIR* dir = opendir(src_path);
    while (dir == NULL && errno == EMFILE) {//waiting for a descriptor
        nanosleep(&sleep_time, NULL);
        dir = opendir(src_path);   
    }
    if (dir == NULL) {
        perror(src_path);
        pthread_exit(0);
    }
    pthread_cleanup_push(close_dir, dir);  

    char* src_dirname = get_name(src_path);
    char* dst_newpath = malloc(strlen(dst_path) + strlen(src_dirname) + 2);
    pthread_cleanup_push(free, dst_newpath);
    create_path(dst_newpath, dst_path, src_dirname);
    struct stat src_stat;
    struct stat dst_stat;
    if(stat(src_path, &src_stat) != 0) { //getting st_mode to create dir
        perror(src_path);
        pthread_exit(0);
    }
    if (mkdir(dst_newpath, src_stat.st_mode) != 0) { //creating dir
        if (errno != EEXIST || stat(dst_newpath, &dst_stat) != 0 || (dst_stat.st_mode & S_IFMT) != S_IFDIR) { //if file located in dst_newpath exists and it is a directory do nothing, else print error
               perror(dst_newpath);
               //free(dst_newpath);
               pthread_exit(0);
           }
    }
    struct dirent* entry = malloc(sizeof(struct dirent) + pathconf(src_path, _PC_NAME_MAX) + 1);
    struct dirent** entry_ptr = &entry;//malloc(sizeof(struct dirent*));
    pthread_t tid;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        perror("Error creating pthread attributes");
        pthread_exit(0);
    }
    pthread_cleanup_push(destroy_attr, &attr);
    errno = 0;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    while ( readdir_r(dir, entry, entry_ptr) == 0 && *entry_ptr != NULL) {//reading entries
        if (strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, ".") == 0) {//check if entry is . or .., it loops if allows . or ..
            continue;
        }

        struct stat statbuf;
        char* src_newpath = malloc(strlen(src_path) + strlen(entry->d_name) + 2);
        pthread_cleanup_push(free, src_newpath);
        create_path(src_newpath, src_path, entry->d_name);       

        if(stat(src_newpath, &statbuf) != 0) {
              perror(src_newpath);
              free(src_newpath);
              continue;
        }

        void* (*func_ptr)(void*);
        if ((statbuf.st_mode & S_IFMT) == S_IFREG) { // S_ISREG(statbuf.st_mode)
            func_ptr = copy_regfile;   
        } else if ((statbuf.st_mode & S_IFMT) == S_IFDIR) {
            func_ptr = copy_dir;
        } else {
            free(src_newpath);
            continue;
        }
        char* dst_newpath_dup = strdup(dst_newpath);
        pthread_cleanup_push(free, dst_newpath_dup);
        char** path_args = malloc(2 * sizeof(char*));
        pthread_cleanup_push(free, path_args);
        path_args[0] = src_newpath;
        path_args[1] = dst_newpath_dup;
        int ret = 0;
        if ((ret = pthread_create(&tid, &attr, func_ptr, (void*)path_args)) != 0) {
            perror("Error creating thread");
            pthread_exit(0);
        }
        pthread_cleanup_pop(0);//if everything is fine, don't free resources, they will be freed in the child thread
        pthread_cleanup_pop(0);
        pthread_cleanup_pop(0); 
    } 
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    pthread_exit(0);
}


size_t last_notslash(char* str) {
    size_t str_len = strlen(str);
    while (str[str_len - 1] == '/') {
        str_len--;
    }
    return str_len;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s [absolute path to directory] [absolute destination path]\n", argv[0]);
        pthread_exit(0);
    }
    size_t src_path_len = last_notslash(argv[1]);
    size_t dst_path_len = last_notslash(argv[2]);
    char* src_path = strndup(argv[1], src_path_len);
    if (src_path == NULL) {
        perror("Error duplicating path");
        pthread_exit(0);
    }
    char* dst_path = strndup(argv[2], dst_path_len);
    if (dst_path == NULL) {
        perror("Error duplicating path");
        free(src_path);
        pthread_exit(0);
    }
    char** args = malloc(2 * sizeof(char*));
    if (args == NULL) {
        perror("Error allocating memory");
        free(src_path);
        free(dst_path);
        pthread_exit(0);
    }
    args[0] = src_path;
    args[1] = dst_path;
    pthread_t pid;
    if (pthread_create(&pid, NULL, copy_dir, args) != 0) {
       perror("Error creating thread");
       free(src_path);
       free(dst_path);
       free(args);
    } 
    pthread_exit(0);
}

