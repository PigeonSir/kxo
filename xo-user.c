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
#define XO_DEVICE_FILE "/dev/kxo0"
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

char draw_buffer[DRAWBUFFER_SIZE];
char table[MAX_GAME][N_GRIDS];

// /* Draw the board into draw_buffer */
// static int draw_board(int id, int win)
// {
//     int i = 0, k = 0;
//     draw_buffer[i++] = '\n';
//     draw_buffer[i++] = '\n';

//     while (i < DRAWBUFFER_SIZE) {
//         for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
//             draw_buffer[i++] = j & 1 ? '|' : table[id][k++];
//         }
//         draw_buffer[i++] = '\n';
//         for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
//             draw_buffer[i++] = '-';
//         }
//         draw_buffer[i++] = '\n';
//     }

//     if (win != -1) {
//         printf("\n");
//         char winner = win == 1 ? 'X' : 'O';
//         printf(" game %d : %c win !!!", id, winner);
//     }

//     return 0;
// }

// /* Insert the whole chess board into the kfifo buffer */
// static void produce_board(int id, unsigned short data)
// {
//     unsigned short win = data >> 15 & 1;
//     unsigned short turn_num = data >> 14 & 1;
//     char turn = turn_num ? 'X' : 'O';
//     if (win) {
//         draw_board(id, turn_num);
//         memset(table[id], ' ', N_GRIDS);
//         return;
//     }
//     int move = data & 0xFF;
//     table[id][move] = turn;
//     draw_board(id, false);
//     return;
// }

int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // unsigned short read_buf[DRAWBUFFER_SIZE];

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            // printf("\033[H\033[J");
            /* ASCII escape code to clear the screen */
            // u_int16_t buf[32];
            // 最多一次讀 32 筆 move
            u_int16_t buf = 0;
            ssize_t n = read(device_fd, &buf, sizeof(buf));
            if (n < 0)
                perror("read");
            if (n == 0)
                continue;
            // for (int i = 0; i < n / 2; i++) {
            u_int16_t data = buf;
            bool win = (data >> 15) & 1;
            char turn = (data >> 14) & 1 ? 'X' : 'O';
            u_int8_t game = (data >> 8) & 0x3F;
            u_int8_t move = data & 0xFF;
            printf("raw data : %x -> win=%d turn=%c game=%d move=%d\n", buf,
                   win, turn, game, move);
            // }
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}