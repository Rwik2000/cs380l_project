#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <liburing.h>

#define QD  8
#define BS (10 * 1024 * 1024)

static int infd, outfd;

struct io_data {
    int read;
    off_t first_offset, offset;
    size_t first_len;
    struct iovec iov;
};

static int setup_context(unsigned entries, struct io_uring *ring) {
    int ret = io_uring_queue_init(entries, ring, 0);
    if (ret < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-ret));
        return -1;
    }
    return 0;
}

static int get_file_size(int fd, off_t *size) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (S_ISREG(st.st_mode)) {
        *size = st.st_size;
        return 0;
    } else if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) return -1;
        *size = bytes;
        return 0;
    }
    return -1;
}

static void queue_prepped(struct io_uring *ring, struct io_data *data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    assert(sqe);

    if (data->read)
        io_uring_prep_readv(sqe, infd, &data->iov, 1, data->offset);
    else
        io_uring_prep_writev(sqe, outfd, &data->iov, 1, data->offset);

    io_uring_sqe_set_data(sqe, data);
}

static int queue_read(struct io_uring *ring, off_t size, off_t offset) {
    struct io_data *data = malloc(size + sizeof(*data));
    if (!data) return 1;

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        free(data);
        return 1;
    }

    data->read = 1;
    data->offset = data->first_offset = offset;
    data->iov.iov_base = data + 1;
    data->iov.iov_len = size;
    data->first_len = size;

    io_uring_prep_readv(sqe, infd, &data->iov, 1, offset);
    io_uring_sqe_set_data(sqe, data);
    return 0;
}

static void queue_write(struct io_uring *ring, struct io_data *data) {
    data->read = 0;
    data->offset = data->first_offset;
    data->iov.iov_base = data + 1;
    data->iov.iov_len = data->first_len;

    queue_prepped(ring, data);
    io_uring_submit(ring);
}

static int move_file(struct io_uring *ring, const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) < 0) {
        perror("stat");
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        // Create the destination directory
        if (mkdir(dst, st.st_mode) < 0 && errno != EEXIST) {
            perror("mkdir");
            return 1;
        }

        // Open the source directory
        DIR *dir = opendir(src);
        if (!dir) {
            perror("opendir");
            return 1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char src_path[PATH_MAX], dst_path[PATH_MAX];
            snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
            snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

            // Recursively move files and directories
            if (move_file(ring, src_path, dst_path) != 0) {
                closedir(dir);
                return 1;
            }
        }

        closedir(dir);

        // Remove the empty source directory
        if (rmdir(src) < 0) {
            perror("rmdir");
            return 1;
        }
    } else if (S_ISREG(st.st_mode)) {
        // Open source and destination files
        infd = open(src, O_RDONLY);
        if (infd < 0) {
            perror("open source file");
            return 1;
        }

        outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
        if (outfd < 0) {
            perror("open destination file");
            close(infd);
            return 1;
        }

        // Get file size and copy
        off_t insize;
        if (get_file_size(infd, &insize)) {
            perror("get_file_size");
            close(infd);
            close(outfd);
            return 1;
        }

        unsigned long reads = 0, writes = 0;
        off_t write_left = insize, offset = 0;

        while (insize || write_left) {
            int had_reads = reads, ret;
            while (insize) {
                off_t this_size = insize > BS ? BS : insize;

                if (reads + writes >= QD) break;

                if (queue_read(ring, this_size, offset)) break;

                insize -= this_size;
                offset += this_size;
                reads++;
            }

            if (had_reads != reads) {
                ret = io_uring_submit(ring);
                if (ret < 0) {
                    fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
                    close(infd);
                    close(outfd);
                    return 1;
                }
            }

            // Process completion
            struct io_uring_cqe *cqe;
            while (write_left) {
                ret = io_uring_wait_cqe(ring, &cqe);
                if (ret < 0) {
                    fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
                    close(infd);
                    close(outfd);
                    return 1;
                }

                struct io_data *data = io_uring_cqe_get_data(cqe);
                if (cqe->res < 0) {
                    fprintf(stderr, "cqe failed: %s\n", strerror(-cqe->res));
                    close(infd);
                    close(outfd);
                    return 1;
                }

                if (data->read) {
                    queue_write(ring, data);
                    write_left -= data->first_len;
                    reads--;
                    writes++;
                } else {
                    free(data);
                    writes--;
                }
                io_uring_cqe_seen(ring, cqe);
            }
        }

        close(infd);
        close(outfd);

        // Delete the source file
        if (S_ISDIR(st.st_mode)) {
            // Remove directory
            if (rmdir(src) < 0) {
                perror("rmdir");
                return 1;
            }
        } else {
            // Remove file
            if (unlink(src) < 0) {
                perror("unlink");
                return 1;
            }
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    struct io_uring ring;

    if (argc < 3) {
        printf("Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    if (setup_context(QD, &ring)) return 1;

    if (move_file(&ring, argv[1], argv[2]) != 0) {
        io_uring_queue_exit(&ring);
        return 1;
    }

    io_uring_queue_exit(&ring);
    return 0;
}
