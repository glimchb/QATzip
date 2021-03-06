/***************************************************************************
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2007-2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***************************************************************************/

static char const *const g_license_msg[] = {
    "Copyright (C) 2017 Intel Corporation.",
    0
};

static char const *const g_version_str = "v0.2.3";

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <assert.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <qz_utils.h>
#include <qatzip.h> /* new QATzip interface */
#include <cpa_dc.h>
#include <pthread.h>
#include <qatzipP.h>

/* Estimate maximum data expansion after decompression */
#define DECOMP_BUFSZ_EXPANSION 5

/* Return codes from qzip */
#define OK      0
#define ERROR   1

#define MAX_PATH_LEN   1024 /* max pathname length */
#define SUFFIX ".gz"
#define SFXLEN 3

#define SRC_BUFF_LEN         (512 * 1024 * 1024)

typedef struct RunTimeList_S {
    struct timeval time_s;
    struct timeval time_e;
    struct RunTimeList_S *next;
} RunTimeList_T;

static char *g_program_name = NULL; /* program name */
static int g_decompress = 0;        /* g_decompress (-d) */
static int g_keep = 0;              /* keep (don't delete) input files */
static QzSession_T g_sess;
static QzSessionParams_T g_params_th = {(QzHuffmanHdr_T)0,};

/* Command line options*/
static char const g_short_opts[] = "A:H:L:C:dhkV";
static const struct option g_long_opts[] = {
    /* { name  has_arg  *flag  val } */
    {"decompress", 0, 0, 'd'}, /* decompress */
    {"uncompress", 0, 0, 'd'}, /* decompress */
    {"help",       0, 0, 'h'}, /* give help */
    {"keep",       0, 0, 'k'}, /* keep (don't delete) input files */
    {"version",    0, 0, 'V'}, /* display version number */
    {"algorithm",  1, 0, 'A'}, /* set algorithm type */
    {"huffmanhdr", 1, 0, 'H'}, /* set huffman header type */
    {"level",      1, 0, 'L'}, /* set compression level */
    {"chunksz",    1, 0, 'C'}, /* set chunk size */
    { 0, 0, 0, 0 }
};

const unsigned int USDM_ALLOC_MAX_SZ = (2 * 1024 * 1024 - 5 * 1024);
static void processFile(QzSession_T *sess, const char *iname, int is_compress);

static void tryHelp(void)
{
    QZ_PRINT("Try `%s --help' for more information.\n", g_program_name);
    exit(ERROR);
}

static void help(void)
{
    static char const *const help_msg[] = {
        "Compress or uncompress FILEs (by default, compress FILES in-place).",
        "",
        "Mandatory arguments to long options are mandatory for short options too.",
        "",
        "  -A, --algorithm   set algorithm type",
        "  -d, --decompress  decompress",
        "  -h, --help        give this help",
        "  -H, --huffmanhdr  set huffman header type",
        "  -k, --keep        keep (don't delete) input files",
        "  -V, --version     display version number",
        "  -L, --level       set compression level",
        "  -C, --chunksz     set chunk size",
        0
    };
    char const *const *p = help_msg;

    QZ_PRINT("Usage: %s [OPTION]... [FILE]...\n", g_program_name);
    while (*p) {
        QZ_PRINT("%s\n", *p++);
    }
}

void freeTimeList(RunTimeList_T *time_list)
{
    RunTimeList_T *time_node = time_list;
    RunTimeList_T *pre_time_node = NULL;

    while (time_node) {
        pre_time_node = time_node;
        time_node = time_node->next;
        free(pre_time_node);
    }
}

static void displayStats(RunTimeList_T *time_list,
                         unsigned insize, unsigned outsize, int is_compress)
{
    /* Calculate time taken (from begin to end) in micro seconds */
    unsigned long us_begin = 0;
    unsigned long us_end = 0;
    double us_diff = 0;
    RunTimeList_T *time_node = time_list;

    while (time_node) {
        us_begin = time_node->time_s.tv_sec * 1000000 + time_node->time_s.tv_usec;
        us_end = time_node->time_e.tv_sec * 1000000 + time_node->time_e.tv_usec;
        us_diff += (us_end - us_begin);
        time_node = time_node->next;
    }

    assert(0 != us_diff);
    assert(0 != insize);
    assert(0 != outsize);
    double size = (is_compress) ? insize : outsize;
    double throughput = (size * CHAR_BIT) / us_diff; /* in MB (megabytes) */
    double compressionRatio = ((double)insize) / ((double)outsize);
    double spaceSavings = 1 - ((double)outsize) / ((double)insize);

    QZ_PRINT("Time taken:    %9.3lf ms\n", us_diff / 1000);
    QZ_PRINT("Throughput:    %9.3lf Mbit/s\n", throughput);
    if (is_compress) {
        QZ_PRINT("Space Savings: %9.3lf %%\n", spaceSavings * 100.0);
        QZ_PRINT("Compression ratio: %.3lf : 1\n", compressionRatio);
    }
}

static int doProcessBuffer(QzSession_T *sess,
                           unsigned char *src, unsigned int *src_len,
                           unsigned char *dst, unsigned int dst_len,
                           RunTimeList_T *time_list, FILE *dst_file,
                           unsigned int *dst_file_size, int is_compress)
{
    int ret = QZ_FAIL;
    unsigned int done = 0;
    unsigned int buf_processed = 0;
    unsigned int buf_remaining = *src_len;
    unsigned int bytes_written = 0;
    unsigned int valid_dst_buf_len = dst_len;
    RunTimeList_T *time_node = time_list;

    puts((is_compress) ? "Compressing..." : "Decompressing...");

    while (time_node->next) {
        time_node = time_node->next;
    }

    while (!done) {
        RunTimeList_T *run_time = calloc(1, sizeof(RunTimeList_T));
        assert(NULL != run_time);
        run_time->next = NULL;
        time_node->next = run_time;
        time_node = run_time;

        gettimeofday(&run_time->time_s, NULL);

        /* Do actual work */
        if (is_compress) {
            ret = qzCompress(sess, src, src_len, dst, &dst_len, 1);
        } else {
            ret = qzDecompress(sess, src, src_len, dst, &dst_len);

            if (QZ_DATA_ERROR == ret) {
                done = 1;
            }
        }

        if (ret != QZ_OK &&
            ret != QZ_BUF_ERROR &&
            ret != QZ_DATA_ERROR) {
            const char *op = (is_compress) ? "Compression" : "Decompression";
            QZ_ERROR("doProcessBuffer:%s failed with error: %d\n", op, ret);
            break;
        }

        gettimeofday(&run_time->time_e, NULL);

        bytes_written = fwrite(dst, 1, dst_len, dst_file);
        assert(bytes_written == dst_len);
        *dst_file_size += bytes_written;

        buf_processed += *src_len;
        buf_remaining -= *src_len;
        if (0 == buf_remaining) {
            done = 1;
        }
        src += *src_len;
        QZ_DEBUG("src_len is %u ,buf_remaining is %u\n", *src_len, buf_remaining);
        *src_len = buf_remaining;
        dst_len = valid_dst_buf_len;
        bytes_written = 0;
    }

    *src_len = buf_processed;
    return ret;
}

void doProcessFile(QzSession_T *sess, const char *src_file_name,
                   const char *dst_file_name, int is_compress)
{
    int ret = OK;
    struct stat src_file_stat;
    unsigned int src_buffer_size = 0, src_file_size = 0;
    unsigned int dst_buffer_size = 0, dst_file_size = 0;
    unsigned int file_remaining = 0;
    unsigned char *src_buffer = NULL;
    unsigned char *dst_buffer = NULL;
    FILE *src_file = NULL;
    FILE *dst_file = NULL;
    unsigned int bytes_read = 0, bytes_processed = 0;
    const off_t max_file_size = UINT_MAX;
    RunTimeList_T *time_list_head = malloc(sizeof(RunTimeList_T));
    assert(NULL != time_list_head);
    gettimeofday(&time_list_head->time_s, NULL);
    time_list_head->time_e = time_list_head->time_s;
    time_list_head->next = NULL;

    ret = stat(src_file_name, &src_file_stat);
    if (ret) {
        perror(src_file_name);
        exit(ERROR);
    }
    if (src_file_stat.st_size > max_file_size) {
        QZ_ERROR("Input file size %zu bytes is greater than the "
                 "currently supported maximum %zu bytes (~%zuGiB)\n",
                 src_file_stat.st_size, max_file_size,
                 (max_file_size + 1) / (1024 * 1024 * 1024));
        exit(ERROR);
    }

    src_file_size = GET_LOWER_32BITS(src_file_stat.st_size);
    src_buffer_size = (src_file_size > SRC_BUFF_LEN) ? SRC_BUFF_LEN : src_file_size;
    if (is_compress) {
        dst_buffer_size = qzMaxCompressedLength(src_buffer_size);
    } else { /* decompress */
        dst_buffer_size = src_buffer_size * DECOMP_BUFSZ_EXPANSION;
    }

    src_buffer = malloc(src_buffer_size);
    assert(src_buffer != NULL);
    dst_buffer = malloc(dst_buffer_size);
    assert(dst_buffer != NULL);
    src_file = fopen(src_file_name, "r");
    assert(src_file != NULL);

    dst_file = fopen(dst_file_name, "w");
    assert(dst_file != NULL);

    file_remaining = src_file_size;
    while (file_remaining) {
        bytes_read = fread(src_buffer, 1, src_buffer_size, src_file);
        QZ_PRINT("Reading input file %s (%u Bytes)\n", src_file_name, bytes_read);

        ret = doProcessBuffer(sess, src_buffer, &bytes_read, dst_buffer,
                              dst_buffer_size, time_list_head, dst_file,
                              &dst_file_size, is_compress);

        if (QZ_DATA_ERROR == ret) {
            bytes_processed += bytes_read;
            if (0 == bytes_read ||
                -1 == fseek(src_file, bytes_processed, SEEK_SET)) {
                ret = ERROR;
                goto exit;
            }
        } else if (QZ_OK != ret) {
            ret = ERROR;
            goto exit;
        }

        file_remaining -= bytes_read;
    }

    displayStats(time_list_head, src_file_size, dst_file_size, is_compress);

exit:
    freeTimeList(time_list_head);
    fclose(src_file);
    fclose(dst_file);
    free(src_buffer);
    free(dst_buffer);
    if (!g_keep) {
        unlink(src_file_name);
    }
    if (ret) {
        exit(ret);
    }
}

int qatzipSetup(QzSession_T *sess, QzSessionParams_T *params)
{
    int status;

    QZ_DEBUG("mw>>> sess=%p\n", sess);
    status = qzInit(sess, getSwBackup(sess));
    if (QZ_OK != status && QZ_DUPLICATE != status && QZ_NO_HW != status) {
        QZ_ERROR("QAT init failed with error: %d\n", status);
        exit(ERROR);
    }
    QZ_DEBUG("QAT init OK with error: %d\n", status);

    status = qzSetupSession(sess, params);
    if (QZ_OK != status && QZ_DUPLICATE != status && QZ_NO_HW != status) {
        QZ_ERROR("Session setup failed with error: %d\n", status);
        exit(ERROR);
    }

    QZ_DEBUG("Session setup OK with error: %d\n", status);
    return 0;
}

int qatzipClose(QzSession_T *sess)
{
    qzTeardownSession(sess);
    qzClose(sess);

    return 0;
}

bool hasSuffix(const char *fname, int is_compress)
{
    size_t len = strlen(fname);

    if (is_compress) {
        if (len < SFXLEN) {
            return 0;
        } else {
            return !strcmp(fname + (len - SFXLEN), SUFFIX);
        }
    } else {
        return len <= SFXLEN || !strcmp(fname + (len - SFXLEN), SUFFIX);
    }
}

int makeOutName(const char *in_name, char *out_name, int is_compress)
{
    if (is_compress) {
        if (hasSuffix(in_name, is_compress)) {
            QZ_ERROR("Warning: %s already has .gz suffix -- unchanged\n", in_name);
            return -1;
        }
        /* add suffix */
        snprintf(out_name, MAX_PATH_LEN, "%s%s", in_name, SUFFIX);
    } else {
        if (!hasSuffix(in_name, is_compress)) {
            return -1;
        }
        /* remove suffix */
        snprintf(out_name, MAX_PATH_LEN, "%s", in_name);
        out_name[strlen(in_name) - SFXLEN] = '\0';
    }

    return 0;
}

/* Makes a complete file system path by adding a file name to the path of its
 * parent directory. */
void mkPath(char *path, const char *dirpath, char *file)
{
    size_t len;

    len = strlen(dirpath);

    if (len < MAX_PATH_LEN && strlen(file) < MAX_PATH_LEN - len) {
        snprintf(path, MAX_PATH_LEN, "%s/%s", dirpath, file);
    } else {
        assert(0);
    }
}


static void processDir(QzSession_T *sess, const char *iname, int is_compress)
{
    DIR *dir;
    struct dirent *entry;
    char inpath[MAX_PATH_LEN];

    dir = opendir(iname);
    assert(dir);

    while ((entry = readdir(dir))) {
        /* Ignore anything starting with ".", which includes the special
         * files ".", "..", as well as hidden files. */
        if (entry->d_name[0] == '.') {
            continue;
        }

        /* Qualify the file with its parent directory to obtain a complete
         * path. */
        mkPath(inpath, iname, entry->d_name);

        processFile(sess, inpath, is_compress);
    }
}

static void processFile(QzSession_T *sess, const char *iname, int is_compress)
{
    int ret;
    struct stat fstat;

    ret = stat(iname, &fstat);
    if (ret) {
        perror(iname);
        exit(-1);
    }

    if (S_ISDIR(fstat.st_mode)) {
        processDir(sess, iname, is_compress);
    } else {
        char oname[MAX_PATH_LEN];
        memset(oname, 0, MAX_PATH_LEN);

        if (makeOutName(iname, oname, is_compress)) {
            return;
        }
        doProcessFile(sess, iname, oname, is_compress);
    }
}

static void version()
{
    char const *const *p = g_license_msg;

    QZ_PRINT("%s %s\n", g_program_name, g_version_str);
    while (*p) {
        QZ_PRINT("%s\n", *p++);
    }
}

static char *qzipBaseName(char *fname)
{
    char *p;

    if ((p = strrchr(fname, '/')) != NULL) {
        fname = p + 1;
    }
    return fname;
}

int main(int argc, char **argv)
{
    int file_count; /* number of files to process */
    g_program_name = qzipBaseName(argv[0]);

    if (qzGetDefaults(&g_params_th) != QZ_OK)
        return -1;

    while (true) {
        int optc;
        int long_idx = -1;
        char *stop = NULL;

        optc = getopt_long(argc, argv, g_short_opts, g_long_opts, &long_idx);
        if (optc < 0) {
            break;
        }
        switch (optc) {
        case 'd':
            g_decompress = 1;
            break;
        case 'h':
            help();
            exit(OK);
            break;
        case 'k':
            g_keep = 1;
            break;
        case 'V':
            version();
            exit(OK);
            break;
        case 'A':
            if (strcmp(optarg, "deflate") == 0) {
                g_params_th.comp_algorithm = QZ_DEFLATE;
            } else if (strcmp(optarg, "snappy") == 0) {
                g_params_th.comp_algorithm = QZ_SNAPPY;
            } else if (strcmp(optarg, "lz4") == 0) {
                g_params_th.comp_algorithm = QZ_LZ4;
            } else {
                QZ_ERROR("Error service arg: %s\n", optarg);
                return -1;
            }
            break;
        case 'H':
            if (strcmp(optarg, "static") == 0) {
                g_params_th.huffman_hdr = QZ_STATIC_HDR;
            } else if (strcmp(optarg, "dynamic") == 0) {
                g_params_th.huffman_hdr = QZ_DYNAMIC_HDR;
            } else {
                QZ_ERROR("Error huffman arg: %s\n", optarg);
                return -1;
            }
            break;
        case 'L':
            g_params_th.comp_lvl = GET_LOWER_32BITS(strtoul(optarg, &stop, 0));
            if (*stop != '\0' || errno ||  \
                g_params_th.comp_lvl > 9 || g_params_th.comp_lvl <= 0) {
                QZ_ERROR("Error compLevel arg: %s\n", optarg);
                return -1;
            }
            break;
        case 'C':
            g_params_th.hw_buff_sz = GET_LOWER_32BITS(strtoul(optarg, &stop, 0));
            if (*stop != '\0' || errno || g_params_th.hw_buff_sz > USDM_ALLOC_MAX_SZ / 2) {
                printf("Error chunk size arg: %s\n", optarg);
                return -1;
            }
            break;
        default:
            tryHelp();
        }
    }

    file_count = argc - optind;
    if (0 == file_count) {
        help();
        exit(OK);
    }

    if (g_decompress) {
        g_params_th.direction = QZ_DIR_DECOMPRESS;
    } else {
        g_params_th.direction = QZ_DIR_COMPRESS;
    }

    if (qatzipSetup(&g_sess, &g_params_th)) {
        exit(ERROR);
    }

    while (optind < argc) {
        processFile(&g_sess, argv[optind++], g_decompress == 0);
    }

    if (qatzipClose(&g_sess)) {
        exit(ERROR);
    }

    return 0;
}
