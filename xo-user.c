#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("\n\nStopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("\n\nStopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}

char table[MAX_GAME][N_GRIDS];
int fds[MAX_GAME];

/* Draw the board into draw_buffer */
static int draw_board(int id, unsigned short data)
{
    printf("\n");
    unsigned short win = (data >> 15) & 1;
    char turn = (data >> 14) & 1 ? 'X' : 'O';
    unsigned short game = (data >> 8) & 0x3F;
    unsigned short move = data & 0xFF;

    printf("raw data : %x -> win=%d turn=%c game=%d move=%d\n", data, win, turn,
           game, move);
    if (win == 1) {
        printf("\n");
        printf(" game %d : %c win !!!", id, turn);
        memset(table[id], ' ', N_GRIDS);
        return 0;
    }

    table[id][move] = turn;

    int k = 0;
    for (int row = 0; row < BOARD_SIZE; row++) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            if (j & 1)
                printf("|");
            else
                printf("%c", table[id][k++]);
        }
        printf("\n");
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            printf("-");
        }
        printf("\n");
    }

    if (win) {
        printf("\n");
        printf(" game %d : %c win !!!", id, turn);
        memset(table[id], ' ', N_GRIDS);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    int max_fd = -1;
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    for (int i = 0; i < MAX_GAME; i++) {
        memset(table[i], ' ', N_GRIDS);
        char path[32];
        snprintf(path, sizeof(path), "%s%d", XO_DEVICE_FILE, i);
        fds[i] = open(path, O_RDONLY);
        max_fd = fds[i] > STDIN_FILENO ? fds[i] : STDIN_FILENO;
    }

    fd_set readset;
    read_attr = true;
    end_attr = false;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        for (int i = 0; i < MAX_GAME; i++)
            FD_SET(fds[i], &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr) {
            for (int i = 0; i < MAX_GAME; i++) {
                if (FD_ISSET(fds[i], &readset)) {
                    FD_CLR(fds[i], &readset);
                    // printf("\033[H\033[J");
                    /* ASCII escape code to clear the screen */
                    unsigned char buf[2];
                    ssize_t n = read(fds[i], buf, 2);
                    if (n < 0)
                        perror("read");
                    if (n == 0)
                        continue;
                    unsigned short data =
                        buf[0] | ((unsigned short) buf[1] << 8);
                    draw_board(i, data);
                }
            }
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);
    for (int i = 0; i < MAX_GAME; i++)
        close(fds[i]);

    return 0;
}