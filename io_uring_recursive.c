#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUF 256
#define QD 256
#define BS 128 * 1024
#define FALLOCATE_THRESHOLD BS

struct io_uring ring;

enum io_operation {
    OP_FALLOCATE,
    OP_READ,
    OP_WRITE
};

// represents a copy request from infd to outfd
// maintains the number of reads on infd and number of writes on outfd
// must use the same copy_data for different queue jobs on the same file
struct copy_data {
    int infd, outfd;
    int reads, writes, falloc;
    size_t insize;
    off_t offset;
};

struct io_data {
    enum io_operation operation;
    struct copy_data *cd;
    size_t size;
    off_t offset;
};
// a stack data structure for reusing data buffers
struct io_data *reused_data[QD];
int reused_data_tail = 0;

int setup_context(unsigned entries, struct io_uring *ring) {
    int ret = io_uring_queue_init(entries, ring, 0);
    if (ret < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-ret));
        return -1;
    }
    return 0;
}

// Prep a queue job
void queue_prep(struct io_uring *ring, struct io_data *data) {
    struct io_uring_sqe *sqe;

    sqe = io_uring_get_sqe(ring);
    assert(sqe);
    assert(data);

    switch (data->operation) {
        case OP_READ:
            io_uring_prep_read(sqe, data->cd->infd, data + 1, data->size, data->offset);
            break;
        case OP_WRITE:
            io_uring_prep_write(sqe, data->cd->outfd, data + 1, data->size, data->offset);
            break;
        case OP_FALLOCATE:
            io_uring_prep_fallocate(sqe, data->cd->outfd, 0, data->offset, data->size);
            break;
    }

    io_uring_sqe_set_data(sqe, data);
}

// Create a queue job that copies from infd to outfd @ size and offset
struct io_data *queue_create(struct copy_data *cd, size_t size, off_t offset, enum io_operation operation) {
    if (cd == NULL) {
        fprintf(stderr, "current copy_file struct is null\n");
        return NULL;
    }

    // we have to take max size here as we are to reuse these
    struct io_data *data = reused_data_tail ? reused_data[--reused_data_tail] : malloc(sizeof(struct io_data) + sizeof(char) * (BS + 1));
    if (data == NULL) {
        perror("malloc io_data");
        return NULL;
    }

    data->cd = cd;
    data->size = size;
    data->offset = offset;
    data->operation = operation;

    return data;
}

struct copy_data *create_copy_data(char *src, char *dest, size_t size) {
    struct copy_data *cp = malloc(sizeof(struct copy_data));
    if (cp == NULL) {
        perror("malloc copy_data");
        return NULL;
    }

    cp->infd = open(src, O_RDONLY);
    if (cp->infd < 0) {
        perror("open infile");
        return NULL;
    }
    cp->outfd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cp->outfd < 0) {
        perror("open outfile");
        return NULL;
    }
    cp->insize = size;
    cp->offset = 0;
    cp->reads = 0;
    cp->writes = 0;
    cp->falloc = (size >= FALLOCATE_THRESHOLD);

    return cp;
}

int copy_recursive(struct io_uring *ring, char *const src[], char *dest) {
    FTS *ftsp;
    FTSENT *p;

    int fts_options = FTS_LOGICAL | FTS_NOCHDIR;
    if ((ftsp = fts_open(src, fts_options, NULL)) == NULL) {
        warn("fts_open");
        return 1;
    }

    int src_len = strlen(src[0]), dest_len = strlen(dest);
    char *dest_file = (char *)malloc(BUF);
    strcpy(dest_file, dest);

    struct io_uring_cqe *cqe;
    struct copy_data *cur_file = NULL;
    int cur_depth = 0, new_ops, got_comp, ret;
    int end_copy = 0;

    while (cur_depth || !end_copy) {
        // Queue up all the reads
        new_ops = 0;
        while (cur_depth < QD) {
            if (cur_file == NULL) {
                // get the next file to be read
                while (1) {
                    if ((p = fts_read(ftsp)) == NULL) {
                        end_copy = 1;
                        break;
                    }
                    switch (p->fts_info) {
                        case FTS_D:
                            // make a new directory
                            strcpy(dest_file + dest_len, p->fts_path + src_len);
                            mkdir(dest_file, 0700);  // we don't care if this fails, we only need the guarantee that there is a directory after
                            break;

                        case FTS_F:
                            strcpy(dest_file + dest_len, p->fts_path + src_len);
                            cur_file = create_copy_data(p->fts_path, dest_file, p->fts_statp->st_size);
                            break;
                        default:
                            break;
                    }
                    if (cur_file != NULL) {
                        break;
                    }
                }
            }

            if (cur_file == NULL) {
                // no file left to copy
                break;
            }
            if (cur_file->falloc && cur_depth < QD) {
                struct io_data *data = queue_create(cur_file, cur_file->insize, 0, OP_FALLOCATE);

                new_ops = 1;
                cur_depth++;
                cur_file->falloc = 0;

                queue_prep(ring, data);
            }
            while (cur_file->insize && cur_depth < QD) {
                size_t cur_size = cur_file->insize;
                if (cur_size > BS) {
                    cur_size = BS;
                }

                struct io_data *data = queue_create(cur_file, cur_size, cur_file->offset, OP_READ);

                cur_file->insize -= cur_size;
                cur_file->offset += cur_size;
                cur_file->reads++;
                new_ops = 1;
                cur_depth++;

                queue_prep(ring, data);
            }

            if (cur_file->insize == 0) {
                // this might seem like a dangling pointer but it's not
                // we leave the curfile struct allocated so queued jobs
                // can access them
                cur_file = NULL;
            }
        }

        if (new_ops) {
            // submit multiple reads at the same time
            ret = io_uring_submit(ring);
            if (ret < 0) {
                fprintf(stderr, "io_uring_submit: %s\n", strerror(-ret));
                break;
            }
        }

        got_comp = 0;
        while (cur_depth) {
            struct io_data *data;

            if (!got_comp) {
                ret = io_uring_wait_cqe(ring, &cqe);
                got_comp = 1;
            } else {
                ret = io_uring_peek_cqe(ring, &cqe);
                if (ret == -EAGAIN) {
                    cqe = NULL;
                    ret = 0;
                }
            }

            if (ret < 0) {
                fprintf(stderr, "io_uring_peek_cqe: %s\n",
                        strerror(-ret));
                return 1;
            }
            if (!cqe)
                break;

            data = io_uring_cqe_get_data(cqe);
            if (cqe->res < 0) {
                if (cqe->res == -EAGAIN) {
                    queue_prep(ring, data);
                    io_uring_cqe_seen(ring, cqe);
                    continue;
                }
                fprintf(stderr, "cqe failed: %s\n",
                        strerror(-cqe->res));
                return 1;
            } else if (data->operation != OP_FALLOCATE && cqe->res != data->size) {
                /* short read/write; adjust and requeue */
                data->offset += cqe->res;
                data->size -= cqe->res;
                queue_prep(ring, data);
                io_uring_cqe_seen(ring, cqe);
                continue;
            }

            /*
             * All done. If write, nothing else to do. If read,
             * queue up corresponding write.
             * */

            switch (data->operation) {
                case OP_READ:
                    data->operation = OP_WRITE;  // change to write
                    data->cd->reads--;
                    data->cd->writes++;
                    if (!data->cd->reads && !data->cd->insize) {
                        // we don't need any more read on this file
                        // so we close the read fd
                        close(data->cd->infd);
                    }

                    queue_prep(ring, data);
                    int ret = io_uring_submit(ring);
                    if (ret < 0) {
                        perror("io_uring_submit write");
                    }
                    break;
                case OP_WRITE:
                    data->cd->writes--;
                    if (!data->cd->reads && !data->cd->writes && !data->cd->falloc && !data->cd->insize) {
                        // we don't need any more write on this queue
                        // so we close the write fd AND free the copy_data struct
                        close(data->cd->outfd);
                        free(data->cd);
                    }
                case OP_FALLOCATE:
                    // let's reuse this unused data
                    reused_data[reused_data_tail++] = data;
                    cur_depth--;
                    break;
            }
            io_uring_cqe_seen(ring, cqe);
        }
    }

    fts_close(ftsp);
    // let it be free
    while (reused_data_tail--) {
        free(reused_data[reused_data_tail]);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <src> <dest>\n", argv[0]);
        return 1;
    }

    if (setup_context(QD, &ring)) {
        return 1;
    }

    // create new destination position
    char *dest = argv[argc - 1];
    argv[argc - 1] = NULL;

    int rc = copy_recursive(&ring, argv + 1, dest);
    io_uring_queue_exit(&ring);

    return rc;
}
