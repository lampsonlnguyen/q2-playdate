#define _GNU_SOURCE

#include "itch.h"
#include "card.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define HELPER CARD_ROOT "/q2-playdate-itch"

enum { LIMIT = 80, ROWS = 7, ROW_HEIGHT = 23 };

typedef struct {
    char titles[LIMIT][96];
    char urls[LIMIT][256];
    char output[28672];
    char message[100];
    size_t used;
    int count, selected, page, more, fd, downloading, wheel;
    pid_t pid;
} Browser;

static int start(Browser *browser, int download)
{
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return 0;
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    char page[8];
    snprintf(page, sizeof page, "%d", browser->page);
    pid_t parent = getpid();
    pid_t child = fork();
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent ||
            dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (download)
            execl(HELPER, HELPER, "download", browser->urls[browser->selected],
                  CARD_ROOT "/games", (char *)NULL);
        else
            execl(HELPER, HELPER, "list", page, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) {
        close(pipefd[0]);
        return 0;
    }
    browser->fd = pipefd[0];
    browser->pid = child;
    browser->used = 0;
    browser->downloading = download;
    snprintf(browser->message, sizeof browser->message, "%s",
             download ? "Downloading. Return cancels." : "Loading free Playdate games...");
    if (!download)
        browser->count = browser->selected = browser->more = 0;
    return 1;
}

static void read_output(Browser *browser)
{
    char discard[512];
    size_t available = sizeof browser->output - browser->used - 1;
    ssize_t n;
    while ((n = read(browser->fd, available ? browser->output + browser->used : discard,
                     available ? available : sizeof discard)) > 0) {
        if (available) {
            browser->used += (size_t)n;
            available -= (size_t)n;
        }
    }
    browser->output[browser->used] = 0;
}

static void finish(Browser *browser, int success)
{
    close(browser->fd);
    browser->pid = 0;
    char *save;
    char *line = strtok_r(browser->output, "\r\n", &save);
    if (!success) {
        snprintf(browser->message, sizeof browser->message, "%s",
                 line ? line : "Helper unavailable. Check Wi-Fi and card files.");
        return;
    }
    if (browser->downloading) {
        snprintf(browser->message, sizeof browser->message, "Added to your library");
        return;
    }
    browser->more = line && !strcmp(line, "true");
    while (browser->count < LIMIT && (line = strtok_r(NULL, "\r\n", &save))) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab++ = 0;
        if (strlen(line) >= sizeof browser->urls[0] || !*tab)
            continue;
        strcpy(browser->urls[browser->count], line);
        snprintf(browser->titles[browser->count], sizeof browser->titles[0], "%s", tab);
        browser->count++;
    }
    snprintf(browser->message, sizeof browser->message, "%s",
             browser->count ? "" : "No games on this page");
}

static void stop(Browser *browser)
{
    if (!browser->pid)
        return;
    kill(browser->pid, SIGTERM);
    for (int attempt = 0; attempt < 20; attempt++) {
        pid_t ended = waitpid(browser->pid, NULL, WNOHANG);
        if (ended > 0 || (ended < 0 && errno == ECHILD)) {
            close(browser->fd);
            return;
        }
        poll(NULL, 0, 50);
    }
    kill(browser->pid, SIGKILL);
    while (waitpid(browser->pid, NULL, 0) < 0 && errno == EINTR) {}
    close(browser->fd);
}

static int row_count(const Browser *browser)
{
    return browser->count + (browser->page > 1) + browser->more;
}

static void move(Browser *browser, int step)
{
    int next = browser->selected + step;
    if (next >= 0 && next < row_count(browser))
        browser->selected = next;
}

static void choose(Browser *browser)
{
    int download = browser->selected < browser->count;
    if (!download && row_count(browser)) {
        int next = browser->more && browser->selected == browser->count;
        browser->page += next ? 1 : -1;
    }
    if (!start(browser, download))
        snprintf(browser->message, sizeof browser->message, "Could not start itch.io helper");
}

static void draw(const Browser *browser, uint8_t picture[PICKER_HEIGHT][PICKER_WIDTH])
{
    memset(picture, 0, PICKER_WIDTH * PICKER_HEIGHT);
    picker_label(picture, 16, 12, "GET GAMES / ITCH.IO", 46, 1);
    int first = browser->selected / ROWS * ROWS;
    for (int row = 0; row < ROWS && first + row < row_count(browser); row++) {
        int item = first + row;
        int y = 34 + row * ROW_HEIGHT;
        int selected = item == browser->selected;
        const char *title = item < browser->count ? browser->titles[item] :
            (browser->more && item == browser->count ? "NEXT PAGE" : "PREVIOUS PAGE");
        if (selected)
            for (int i = y - 4; i < y + 15; i++)
                memset(&picture[i][10], 1, 380);
        picker_label(picture, 16, y, title, 46, !selected);
    }
    if (browser->message[0]) {
        picker_label(picture, 16, 198, browser->message, 46, 1);
        if (strlen(browser->message) > 46)
            picker_label(picture, 16, 212, browser->message + 46, 46, 1);
    } else {
        picker_label(picture, 16, 214, "CENTRE DOWNLOAD  RETURN BACK", 46, 1);
    }
}

void itch_browse(int input, ItchPresent present, void *context)
{
    static Browser browser;
    static uint8_t picture[PICKER_HEIGHT][PICKER_WIDTH];
    memset(&browser, 0, sizeof browser);
    browser.page = 1;
    browser.fd = -1;
    if (!start(&browser, 0))
        snprintf(browser.message, sizeof browser.message, "Could not start itch.io helper");

    for (;;) {
        if (browser.pid) {
            read_output(&browser);
            int status;
            pid_t ended = waitpid(browser.pid, &status, WNOHANG);
            if (ended > 0 || (ended < 0 && errno != EINTR)) {
                read_output(&browser);
                finish(&browser, ended > 0 && WIFEXITED(status) && !WEXITSTATUS(status));
            }
        }
        draw(&browser, picture);
        present(context, picture);

        struct pollfd event = {.fd = input, .events = POLLIN};
        int ready = poll(&event, 1, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && !(event.revents & POLLIN)))
            break;
        if (!ready)
            continue;
        char packet[64];
        ssize_t bytes = recv(input, packet, sizeof packet - 1, 0);
        if (bytes <= 0)
            continue;
        packet[bytes] = 0;
        int key, travel, pos, axis;
        if (sscanf(packet, "k u %d", &key) == 1) {
            if (key == 170)
                break;
            if (browser.pid)
                continue;
            if (key == 218)
                choose(&browser);
            else if (key == 172 || key == 222)
                move(&browser, -1);
            else if (key == 173 || key == 223)
                move(&browser, 1);
        } else if (!browser.pid && sscanf(packet, "w %d", &travel) == 1 &&
                   travel >= -2000 && travel <= 2000) {
            browser.wheel -= travel;
            while (browser.wheel >= 26 || browser.wheel <= -26) {
                int direction = browser.wheel > 0 ? 1 : -1;
                move(&browser, direction);
                browser.wheel -= direction * 26;
            }
        } else if (!browser.pid && sscanf(packet, "t u %d %d", &pos, &axis) == 2 &&
                   pos >= GAME_LEFT && pos < GAME_LEFT + GAME_WIDTH &&
                   axis >= GAME_TOP && axis < GAME_TOP + GAME_HEIGHT) {
            int y = (axis - GAME_TOP) * 5 / 4;
            if (y < 30)
                continue;
            int row = (y - 30) / ROW_HEIGHT;
            int item = browser.selected / ROWS * ROWS + row;
            if (row >= 0 && row < ROWS && item < row_count(&browser)) {
                browser.selected = item;
                choose(&browser);
            }
        }
    }
    stop(&browser);
}
