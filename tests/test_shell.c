#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "shell.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define SHELL_IMG "/tmp/minifs_shell_test.img"

static void init_shell_img(shell_t *sh) {
    unlink(SHELL_IMG);
    shell_init(sh);
    strncpy(sh->img_path, SHELL_IMG, sizeof(sh->img_path) - 1);
    ASSERT_OK(shell_execute_line(sh, "mkfs 1024"));
}

TEST(m12_test1_shell_tokenization_and_quotes) {
    shell_t sh;
    init_shell_img(&sh);

    ASSERT_EQ(shell_execute_line(&sh, ""), 0);
    ASSERT_EQ(shell_execute_line(&sh, "   \t  \n"), 0);
    ASSERT_EQ(shell_execute_line(&sh, "# This is a comment"), 0);

    ASSERT_OK(shell_execute_line(&sh, "write \"hello file.txt\" \"Hello, Virtual World!\""));
    ASSERT_OK(shell_execute_line(&sh, "stat \"hello file.txt\""));

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}

TEST(m12_test2_shell_basic_commands) {
    shell_t sh;
    init_shell_img(&sh);

    ASSERT_OK(shell_execute_line(&sh, "mkdir /dir1"));
    ASSERT_OK(shell_execute_line(&sh, "mkdir -p /dir1/sub1/sub2"));
    ASSERT_OK(shell_execute_line(&sh, "cd /dir1/sub1"));
    ASSERT_OK(shell_execute_line(&sh, "pwd"));

    ASSERT_OK(shell_execute_line(&sh, "touch myfile.dat"));
    ASSERT_OK(shell_execute_line(&sh, "write myfile.dat \"Data Content\""));
    ASSERT_OK(shell_execute_line(&sh, "append myfile.dat \" Appended\""));
    ASSERT_OK(shell_execute_line(&sh, "cat myfile.dat"));
    ASSERT_OK(shell_execute_line(&sh, "truncate myfile.dat 4"));

    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&sh.v, "myfile.dat", &st));
    ASSERT_EQ(st.st_size, 4);

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}

TEST(m12_test3_shell_file_ops) {
    shell_t sh;
    init_shell_img(&sh);

    ASSERT_OK(shell_execute_line(&sh, "write orig.txt \"Original File Data\""));
    ASSERT_OK(shell_execute_line(&sh, "cp orig.txt copied.txt"));
    ASSERT_OK(shell_execute_line(&sh, "ln orig.txt linked.txt"));
    ASSERT_OK(shell_execute_line(&sh, "mv copied.txt moved.txt"));

    struct vfs_stat st_orig, st_link, st_moved;
    ASSERT_OK(vfs_stat(&sh.v, "orig.txt", &st_orig));
    ASSERT_OK(vfs_stat(&sh.v, "linked.txt", &st_link));
    ASSERT_OK(vfs_stat(&sh.v, "moved.txt", &st_moved));

    ASSERT_EQ(st_orig.st_ino, st_link.st_ino);
    ASSERT_EQ(st_orig.st_nlink, 2);
    ASSERT_EQ(st_moved.st_size, st_orig.st_size);

    ASSERT_OK(shell_execute_line(&sh, "rm linked.txt"));
    ASSERT_OK(vfs_stat(&sh.v, "orig.txt", &st_orig));
    ASSERT_EQ(st_orig.st_nlink, 1);

    ASSERT_OK(shell_execute_line(&sh, "mkdir -p /deep/tree/dir"));
    ASSERT_OK(shell_execute_line(&sh, "touch /deep/tree/dir/f.txt"));
    ASSERT_OK(shell_execute_line(&sh, "tree /deep"));
    ASSERT_OK(shell_execute_line(&sh, "rm -r /deep"));

    ASSERT_EQ(vfs_stat(&sh.v, "/deep", &st_orig), -ENOENT);

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}

TEST(m12_test4_shell_permissions_and_umask) {
    shell_t sh;
    init_shell_img(&sh);

    ASSERT_OK(shell_execute_line(&sh, "umask 0022"));
    ASSERT_OK(shell_execute_line(&sh, "touch perm.txt"));
    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&sh.v, "perm.txt", &st));
    ASSERT_EQ(st.st_mode & 0777, 0644);

    ASSERT_OK(shell_execute_line(&sh, "chmod 0600 perm.txt"));
    ASSERT_OK(vfs_stat(&sh.v, "perm.txt", &st));
    ASSERT_EQ(st.st_mode & 0777, 0600);

    ASSERT_OK(shell_execute_line(&sh, "chown 500:600 perm.txt"));
    ASSERT_OK(vfs_stat(&sh.v, "perm.txt", &st));
    ASSERT_EQ(st.st_uid, 500);
    ASSERT_EQ(st.st_gid, 600);

    ASSERT_OK(shell_execute_line(&sh, "su 1000 1000"));
    ASSERT_EQ(sh.v.uid, 1000);
    ASSERT_EQ(sh.v.gid, 1000);

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}

TEST(m12_test5_shell_host_transfer) {
    shell_t sh;
    init_shell_img(&sh);

    const char *host_in = "/tmp/minifs_sh_test_in.bin";
    const char *host_out = "/tmp/minifs_sh_test_out.bin";
    unlink(host_in); unlink(host_out);

    /* Create 128 KiB test payload on host */
    size_t payload_sz = 131072;
    char *payload = (char *)malloc(payload_sz);
    ASSERT_TRUE(payload != NULL);
    for (size_t i = 0; i < payload_sz; i++) {
        payload[i] = (char)((i * 37 + 11) & 0xFF);
    }
    int hfd = open(host_in, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ASSERT_TRUE(hfd >= 0);
    ASSERT_EQ(write(hfd, payload, payload_sz), (ssize_t)payload_sz);
    close(hfd);

    /* cp-in */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cp-in %s /imported.bin", host_in);
    ASSERT_OK(shell_execute_line(&sh, cmd));

    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&sh.v, "/imported.bin", &st));
    ASSERT_EQ(st.st_size, payload_sz);

    /* cp-out */
    snprintf(cmd, sizeof(cmd), "cp-out /imported.bin %s", host_out);
    ASSERT_OK(shell_execute_line(&sh, cmd));

    hfd = open(host_out, O_RDONLY);
    ASSERT_TRUE(hfd >= 0);
    char *verified = (char *)malloc(payload_sz);
    ASSERT_TRUE(verified != NULL);
    ASSERT_EQ(read(hfd, verified, payload_sz), (ssize_t)payload_sz);
    close(hfd);

    ASSERT_EQ(memcmp(payload, verified, payload_sz), 0);

    free(payload);
    free(verified);
    unlink(host_in);
    unlink(host_out);

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}

TEST(m12_test6_shell_df_fsck_cache_journal) {
    shell_t sh;
    init_shell_img(&sh);

    ASSERT_OK(shell_execute_line(&sh, "df"));
    ASSERT_OK(shell_execute_line(&sh, "fsck"));
    ASSERT_OK(shell_execute_line(&sh, "journal status"));
    ASSERT_OK(shell_execute_line(&sh, "cache stats"));
    ASSERT_OK(shell_execute_line(&sh, "sync"));
    ASSERT_OK(shell_execute_line(&sh, "help"));

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}

TEST(m12_test7_shell_error_handling) {
    shell_t sh;
    init_shell_img(&sh);

    ASSERT_EQ(shell_execute_line(&sh, "nonexistent_command"), -ENOENT);
    ASSERT_EQ(shell_execute_line(&sh, "cat /does/not/exist"), -ENOENT);
    ASSERT_EQ(shell_execute_line(&sh, "rm /no/such/file"), -ENOENT);
    ASSERT_EQ(shell_execute_line(&sh, "cd /nowhere"), -ENOENT);
    ASSERT_EQ(shell_execute_line(&sh, "mkdir"), -EINVAL);

    /* Test 10,000-char line */
    char *huge = (char *)malloc(10000);
    ASSERT_TRUE(huge != NULL);
    memset(huge, 'A', 9999);
    huge[9999] = '\0';
    ASSERT_EQ(shell_execute_line(&sh, huge), -ENOENT);
    free(huge);

    ASSERT_OK(shell_execute_line(&sh, "exit"));
    ASSERT_TRUE(sh.exit_requested);

    shell_cleanup(&sh);
    unlink(SHELL_IMG);
}
