#define FUSE_USE_VERSION 28

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>

// ini buat nyimpen lokasi folder asli, nanti semua file asli bakal diambil dari folder ini
static char source_dir[1024];

// fungsi yg gabungin source_dir sama path dari fuse
// misal source_dir = /home/user/amba_files
// path = /1.txt
// hasilnya bakal /home/user/amba_files/1.txt
static void make_full_path(char *fullpath, const char *path) {
    snprintf(fullpath, 1024, "%s%s", source_dir, path);
}

// fungsi ini khusus buat bikin isi tujuan.txt, isinya harus dibuat sendiri
static void build_tujuan_content(char *result, size_t result_size) {
    char combined[2048] = "";
    char filepath[2048];
    char line[2048];

    // soal minta fragment diambil urut dari 1.txt sampai 7.txt
    for (int i = 1; i <= 7; i++) {
        snprintf(filepath, sizeof(filepath), "%s/%d.txt", source_dir, i);

        // buka file asli yang ada di amba_files
        FILE *fp = fopen(filepath, "r");

        // kalau filenya gagal dibuka, lewati saja biar program tetap jalan
        if (fp == NULL) {
            continue;
        }

        // baca isi file baris per baris
        while (fgets(line, sizeof(line), fp) != NULL) {
            // yang dicari cuma baris yang diawali KOORD:
            if (strncmp(line, "KOORD:", 6) == 0) {
                // ambil isi setelah tulisan KOORD:
                char *fragment = line + 6;

                // kalau setelah KOORD: ada spasi/tab, dibuang dulu
                while (*fragment == ' ' || *fragment == '\t') {
                    fragment++;
                }

                // hapus enter di akhir baris biar gabungannya rapi
                fragment[strcspn(fragment, "\r\n")] = '\0';

                // masukin fragment ke hasil gabungan
                strncat(combined, fragment, sizeof(combined) - strlen(combined) - 1);

                // kalau KOORD sudah ketemu, tidak perlu baca sisa file
                break;
            }
        }

        fclose(fp);
    }

    snprintf(result, result_size, "Tujuan Mas Amba: %s\n", combined);
}

// getattr dipanggil fuse saat butuh info file/folder, contohnya saat ls -l, stat, atau sebelum file dibaca
static int kenz_getattr(const char *path, struct stat *stbuf) {
    int res;
    char fullpath[1024];

    // kosongin dulu isi struct stat supaya tidak ada data sampah
    memset(stbuf, 0, sizeof(struct stat));

    // kalau yang diminta tujuan.txt, kita kasih atribut manual
    if (strcmp(path, "/tujuan.txt") == 0) {
        char content[4096];

        // isi dibuat dulu supaya ukuran file bisa dihitung
        build_tujuan_content(content, sizeof(content));

        // regular file, read only
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(content);

        return 0;
    }

    // selain tujuan.txt, atributnya langsung ambil dari file asli
    make_full_path(fullpath, path);

    res = lstat(fullpath, stbuf);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

// readdir dipanggil saat user menjalankan ls di folder mount
// tugasnya mengisi daftar file/folder yang kelihatan di mnt
static int kenz_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    DIR *dp;
    struct dirent *de;
    char fullpath[1024];

    // offset dan fi tidak dipakai di program ini
    (void) offset;
    (void) fi;

    // buka folder asli yang sesuai dengan path di mount
    make_full_path(fullpath, path);

    dp = opendir(fullpath);
    if (dp == NULL) {
        return -errno;
    }

    // masukin semua isi folder asli ke tampilan mount, jadi 1.txt sampai 7.txt tetap muncul seperti biasa
    while ((de = readdir(dp)) != NULL) {
        struct stat st;

        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;

        // filler ini yang benar-benar menambahkan nama file ke hasil ls
        if (filler(buf, de->d_name, &st, 0)) {
            break;
        }
    }

    // tujuan.txt cuma ditambahkan di root mount, jadi saat ls mnt, file ini muncul
    // tapi file ini tidak dibuat di amba_files
    if (strcmp(path, "/") == 0) {
        filler(buf, "tujuan.txt", NULL, 0);
    }

    closedir(dp);
    return 0;
}

// open dipanggil saat file mau dibuka, misalnya saat cat mnt/1.txt atau cat mnt/tujuan.txt
static int kenz_open(const char *path, struct fuse_file_info *fi) {
    int fd;
    char fullpath[1024];

    // tujuan.txt virtual, jadi tidak ada file asli yang perlu dibuka
    if (strcmp(path, "/tujuan.txt") == 0) {
        return 0;
    }

    // file selain tujuan.txt dibuka dari source directory
    make_full_path(fullpath, path);

    fd = open(fullpath, fi->flags);
    if (fd == -1) {
        return -errno;
    }

    // cukup cek bisa dibuka atau tidak, lalu tutup lagi, pembacaan isinya dilakukan di fungsi read
    close(fd);
    return 0;
}

// read dipanggil saat isi file benar-benar dibaca, contohnya ketika menjalankan cat mnt/1.txt
static int kenz_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    int fd;
    int res;
    char fullpath[1024];

    // fi tidak dipakai
    (void) fi;

    // kalau yang dibaca tujuan.txt, isi dibuat langsung saat itu juga
    if (strcmp(path, "/tujuan.txt") == 0) {
        char content[4096];

        build_tujuan_content(content, sizeof(content));

        size_t len = strlen(content);

        // kalau offset sudah lewat dari panjang isi, berarti sudah tidak ada data
        if (offset >= len) {
            return 0;
        }

        // kalau size yang diminta terlalu besar, dipotong sesuai sisa data
        if (offset + size > len) {
            size = len - offset;
        }

        // salin isi tujuan.txt ke buffer yang akan dibaca user
        memcpy(buf, content + offset, size);

        // return jumlah byte yang berhasil dibaca
        return size;
    }

    // untuk file biasa, baca langsung dari file asli di amba_files
    make_full_path(fullpath, path);

    fd = open(fullpath, O_RDONLY);
    if (fd == -1) {
        return -errno;
    }

    // pread dipakai supaya bisa membaca dari posisi offset tertentu
    res = pread(fd, buf, size, offset);
    if (res == -1) {
        res = -errno;
    }

    close(fd);
    return res;
}

// daftar fungsi fuse yang dipakai program ini, fuse nanti akan memanggil fungsi-fungsi ini sesuai kebutuhan
static struct fuse_operations kenz_oper = {
    .getattr = kenz_getattr,
    .readdir = kenz_readdir,
    .open = kenz_open,
    .read = kenz_read,
};

int main(int argc, char *argv[]) {
    // butuh 2 argumen: source directory dan mount directory
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <source_dir> <mount_dir>\n", argv[0]);
        return 1;
    }

    // simpan source directory dalam bentuk absolute path, agar program ttp bisa nemu amba_files dari mana pun 
    realpath(argv[1], source_dir);

    // argumen source directory tidak perlu dikirim ke fuse_main, yg dibutuhkan fuse cuma nama program dan mount directory
    argv[1] = argv[0];
    argc--;

    // supaya permission file tidak berubah karena umask sistem
    umask(0);

    // mulai menjalankan filesystem fuse
    return fuse_main(argc, argv + 1, &kenz_oper, NULL);
}
