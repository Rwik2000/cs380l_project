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
#include <time.h>

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

static int sync_file(struct io_uring *ring, const char *src, const char *dst) {
    struct stat src_stat, dst_stat;

    if (stat(src, &src_stat) < 0) {
        perror("stat src");
        return 1;
    }

    int dst_exists = (stat(dst, &dst_stat) == 0);

    // Skip file if it's up-to-date
    if (dst_exists && src_stat.st_size == dst_stat.st_size && src_stat.st_mtime <= dst_stat.st_mtime) {
        printf("Skipping (up-to-date): %s\n", src);
        return 0;
    }

    // Open source and destination files
    infd = open(src, O_RDONLY);
    if (infd < 0) {
        perror("open src");
        return 1;
    }

    outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
    if (outfd < 0) {
        perror("open dst");
        close(infd);
        return 1;
    }

    // Get file size
    off_t insize;
    if (get_file_size(infd, &insize)) {
        perror("get_file_size");
        close(infd);
        close(outfd);
        return 1;
    }

    // Read and write in chunks
    unsigned long reads = 0, writes = 0;
    off_t write_left = insize, offset = 0;

    while (write_left > 0) {
        off_t this_size = (write_left > BS) ? BS : write_left;

        struct io_data *data = malloc(this_size + sizeof(*data));
        if (!data) {
            fprintf(stderr, "Memory allocation failed\n");
            close(infd);
            close(outfd);
            return 1;
        }

        // Prepare the read
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            fprintf(stderr, "io_uring_get_sqe failed\n");
            free(data);
            close(infd);
            close(outfd);
            return 1;
        }

        data->read = 1;
        data->offset = offset;
        data->iov.iov_base = data + 1;
        data->iov.iov_len = this_size;

        io_uring_prep_readv(sqe, infd, &data->iov, 1, offset);
        io_uring_sqe_set_data(sqe, data);

        // Submit the read request
        int ret = io_uring_submit(ring);
        if (ret < 0) {
            fprintf(stderr, "io_uring_submit (read) failed: %s\n", strerror(-ret));
            free(data);
            close(infd);
            close(outfd);
            return 1;
        }

        // Wait for read completion
        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "io_uring_wait_cqe (read) failed: %s\n", strerror(-ret));
            free(data);
            close(infd);
            close(outfd);
            return 1;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Read failed: %s\n", strerror(-cqe->res));
            free(data);
            io_uring_cqe_seen(ring, cqe);
            close(infd);
            close(outfd);
            return 1;
        }

        io_uring_cqe_seen(ring, cqe);

        // Prepare the write
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            fprintf(stderr, "io_uring_get_sqe failed\n");
            free(data);
            close(infd);
            close(outfd);
            return 1;
        }

        data->read = 0;

        io_uring_prep_writev(sqe, outfd, &data->iov, 1, offset);
        io_uring_sqe_set_data(sqe, data);

        // Submit the write request
        ret = io_uring_submit(ring);
        if (ret < 0) {
            fprintf(stderr, "io_uring_submit (write) failed: %s\n", strerror(-ret));
            free(data);
            close(infd);
            close(outfd);
            return 1;
        }

        // Wait for write completion
        ret = io_uring_wait_cqe(ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "io_uring_wait_cqe (write) failed: %s\n", strerror(-ret));
            free(data);
            close(infd);
            close(outfd);
            return 1;
        }

        if (cqe->res < 0) {
            fprintf(stderr, "Write failed: %s\n", strerror(-cqe->res));
            free(data);
            io_uring_cqe_seen(ring, cqe);
            close(infd);
            close(outfd);
            return 1;
        }

        io_uring_cqe_seen(ring, cqe);

        // Update offset and remaining size
        offset += this_size;
        write_left -= this_size;

        free(data);
    }

    close(infd);
    close(outfd);
    printf("Copied: %s -> %s\n", src, dst);
    return 0;
}


static int sync_dir(struct io_uring *ring, const char *src, const char *dst) {
    DIR *dir = opendir(src);
    if (!dir) {
        perror("opendir");
        return 1;
    }

    // Create the destination directory
    struct stat st;
    if (stat(dst, &st) < 0) {
        if (mkdir(dst, 0755) < 0) {
            perror("mkdir");
            closedir(dir);
            return 1;
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char src_path[PATH_MAX], dst_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        // Recursively sync files and directories
        if (entry->d_type == DT_DIR) {
            if (sync_dir(ring, src_path, dst_path) != 0) {
                closedir(dir);
                return 1;
            }
        } else {
            if (sync_file(ring, src_path, dst_path) != 0) {
                closedir(dir);
                return 1;
            }
        }
    }

    closedir(dir);
    return 0;
}

int main(int argc, char *argv[]) {
    struct io_uring ring;
    struct stat st;

    if (argc < 3) {
        printf("Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    // Check if source exists and determine if it's a file or directory
    if (stat(argv[1], &st) < 0) {
        perror("stat source");
        return 1;
    }

    // Initialize io_uring context
    if (setup_context(QD, &ring)) return 1;

    int ret;
    if (S_ISDIR(st.st_mode)) {
        // Source is a directory
        ret = sync_dir(&ring, argv[1], argv[2]);
    } else if (S_ISREG(st.st_mode)) {
        // Source is a regular file
        char dst_path[PATH_MAX];
        snprintf(dst_path, sizeof(dst_path), "%s/%s", argv[2], strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1]);
        ret = sync_file(&ring, argv[1], dst_path);
    } else {
        fprintf(stderr, "Unsupported file type: %s\n", argv[1]);
        ret = 1;
    }

    io_uring_queue_exit(&ring);
    return ret;
}
