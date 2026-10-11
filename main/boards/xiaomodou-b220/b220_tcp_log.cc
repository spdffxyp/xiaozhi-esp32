/*
 * b220_tcp_log.cc - see b220_tcp_log.h.  A debug log channel over TCP so the serial
 * port can be handed to the baseboard - and, in the other direction, a COMMAND channel:
 * every complete line an attached client sends is handed to the local test parser, so a
 * telnet session is both the log and the console.
 *
 * That second direction exists because the first version of this was write-only, and the
 * bench tried to drive the baseboard by typing {BASE+...} into telnet: the bytes arrived,
 * were read, and thrown away (the reader only tested for "did the peer hang up").  A log
 * channel that silently accepts input is worse than one that refuses it, so it now acts on
 * what it is given.
 *
 * DESIGN NOTES THAT MATTER
 * ------------------------
 * 1. The hook runs in WHOEVER's context logged the message - the WiFi stack, the
 *    audio tasks - so it must not block, allocate, or hold a lock for long.  It
 *    formats into a stack buffer, appends to the ring buffer under a short mutex, and
 *    calls the previous handler (which is what keeps the serial log working).  No I/O
 *    happens in the hook: sending is the listener task's job, so a slow or dead client
 *    can never stall the system.
 * 2. The ring buffer keeps the RECENT log, and a newly connected client is sent
 *    everything still in it.  That is what makes the boot log visible to a client that
 *    connects eight seconds in, after WiFi came up.
 * 3. Each client has its own read position, so two can tail at their own pace.  When
 *    the writer laps a client, that client's position is pushed forward: it loses old
 *    lines rather than receiving a torn one.
 * 4. Nothing here logs from its own hot paths.  A log call inside the hook would
 *    recurse, and a log call in the send path can generate more output than the pipe
 *    can carry.
 */

#include "b220_tcp_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_log_write.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_netif.h>

#include <lwip/sockets.h>
#include <netinet/in.h>

/*
 * The local test parser lives with the UART link, because that is where the concept
 * started.  The TCP log only borrows it: one parser means the two channels cannot drift
 * apart in what they accept.
 *
 * This file still does NOT log anything itself - it is the log, and a message from inside
 * it would re-enter the hook below.  Command acknowledgements therefore come from the
 * parser's own logging, not from here.
 */
extern "C" void b220_base_uart_handle_test_command(const char* line);

#define B220_TCP_LOG_PORT        3333
/* The ring size is the amount of history a late client can see.  8 KB is several
 * hundred lines - more than a boot sequence - and irrelevant next to the ~90 KB of
 * free SRAM on this board. */
#define B220_TCP_LOG_RING        8192
/*
 * THE STACK BUDGET, WHICH IS THE DESIGN CONSTRAINT HERE.
 *
 * The formatting buffer has to live on the CALLER's stack: esp_log's va_list is only
 * valid for the duration of the hook, so vsnprintf cannot be deferred.  And the caller is
 * often WiFi's event task, whose stack is CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE - 2304
 * bytes when this module first appeared, with most of it already spoken for.
 *
 * That is not theoretical.  With a 512-byte buffer and the stock 2304-byte event stack the
 * boot died with
 *
 *     ***ERROR*** A stack overflow in task sys_evt has been detected.
 *
 * at "WiFi connecting to dlink" - on a boot that had been fine before this module existed.
 *
 * There were two ways out and this file takes the second:
 *
 *   1. keep the buffer small (256) and accept truncation.  It stopped the overflow but
 *      cut real lines: a measured boot has 4 lines over 256 bytes (of 234) and they are
 *      the board's own DSP bring-up diagnostics - the longest is 466 bytes and its TAIL is
 *      the "per step: 1/5 upload=done, ... 5/5 soft reset=ok" summary, which is the part
 *      actually worth reading.
 *   2. size the buffer to what this firmware emits (512 covers every line) and PAY for it
 *      where it is spent, by raising the event task's stack.  This board's config.json
 *      sets CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE = 3584, i.e. 1.3 KB of heap, against a
 *      fault that took the device down.
 *
 * Streaming a long line as several ring entries was also considered and rejected:
 * vsnprintf has no "resume at offset" form, so it would have meant re-formatting the whole
 * message for every chunk - wrong for a va_list and far more machinery than the problem
 * deserves.
 *
 * The marker below is therefore a backstop, not the normal path: with the buffer at 512
 * and the longest line at 466, nothing in a normal boot reaches it.
 */
#define B220_TCP_LOG_CHUNK       512
/* If a message still does not fit, its tail is replaced with this, so a partial line can
 * never be mistaken for a whole one. */
#define B220_TCP_LOG_CONT_MARK   "...[cut]"
#define B220_TCP_LOG_CONT_LEN    (sizeof(B220_TCP_LOG_CONT_MARK) - 1)
/* Longest single message accepted. */
#define B220_TCP_LOG_MAX_MSG     2048

#define B220_TCP_LOG_MAX_CLIENTS 4
#define B220_TCP_LOG_TASK_STACK  4096
#define B220_TCP_LOG_TASK_PRIO   4
/* select() timeout: how long the task sleeps when nothing is happening. */
#define B220_TCP_LOG_POLL_MS     200

namespace {

class TcpLog {
public:
    static TcpLog& instance()
    {
        static TcpLog inst;
        return inst;
    }

    void start()
    {
        if (started_ || starting_) {
            return;
        }
        starting_ = true;
        ring_mutex_ = xSemaphoreCreateMutex();
        if (ring_mutex_ == nullptr) {
            /* Without the mutex the ring cannot be shared safely, and pretending
             * otherwise would tear the stream.  Stay off: the serial log is untouched
             * either way, so this failure is harmless. */
            return;
        }
        started_ = true;

        /* Install the hook only once the ring is ready, so the hook can rely on it.
         * The previous handler is kept and called for every message - that is what
         * keeps the ordinary serial log working. */
        previous_ = esp_log_set_vprintf(&TcpLog::hook);

        /* The listener task is created even before there is an IP address: bind and
         * listen succeed as soon as the stack is up, and a client simply cannot
         * connect until WiFi has an address.  Doing it here avoids having to watch
         * network events to know when it is safe. */
        (void)xTaskCreate(&TcpLog::task_entry, "b220_tcplog", B220_TCP_LOG_TASK_STACK, this,
                          B220_TCP_LOG_TASK_PRIO, nullptr);
    }

    int clients() const { return client_count_; }

    /* The fallback: print the buffered history to the SERIAL port.  Uses printf()
     * rather than ESP_LOGx on purpose - it must not go back through the hook and
     * append what it is about to print, which would double the log and grow it while
     * printing. */
    void dump()
    {
        if (!started_) {
            printf("[tcplog] log buffer not started\n");
            return;
        }
        size_t len = 0;
        if (xSemaphoreTake(ring_mutex_, portMAX_DELAY) != pdTRUE) {
            return;
        }
        len = (write_pos_ + B220_TCP_LOG_RING - oldest_pos_) % B220_TCP_LOG_RING;
        const int attached = client_count_;

        printf("\n[tcplog] ---- last %u bytes of log (%d client(s) attached) ----\n",
               (unsigned)len, attached);
        /* Printed while holding the ring lock, straight from the ring: a serial dump
         * is a rare, deliberate action, and copying the history into a second 8 KB
         * buffer to avoid that would cost more RAM than the log itself.  The hook
         * cannot run here in a way that matters - it only takes the same lock. */
        for (size_t i = 0; i < len; i++) {
            putchar(ring_[(oldest_pos_ + i) % B220_TCP_LOG_RING]);
        }
        printf("\n[tcplog] ---- end of buffered log ----\n\n");
        xSemaphoreGive(ring_mutex_);
    }

private:
    TcpLog() = default;

    /* ---------------------------------------------------------------- the hook */

    static int hook(const char* fmt, va_list ap)
    {
        TcpLog& self = instance();
        if (self.started_ && self.ring_mutex_ != nullptr) {
            va_list copy;
            va_copy(copy, ap);
            self.ring_printf(fmt, copy);
            va_end(copy);
        }

        /* The original handler, unchanged: this is what still writes the serial log.
         * Called last, so a network client never delays serial output. */
        if (self.previous_ != nullptr) {
            return self.previous_(fmt, ap);
        }
        return vprintf(fmt, ap);
    }

    /*
     * Format into a bounded stack buffer, then copy into the ring.  No null terminator
     * is ever copied into the ring (the stream is raw text), and nothing is written
     * past the end of the buffer.
     *
     * Two passes are not needed: one vsnprintf into the local buffer gives both the text
     * and its length, and vsnprintf cannot overflow that buffer whatever the format
     * asks for.  What it can do is report that the text did not fit, which is when the
     * truncation marker goes on.
     */
    void ring_printf(const char* fmt, va_list ap)
    {
        char buf[B220_TCP_LOG_CHUNK];
        const int written = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (written <= 0) {
            return;
        }
        size_t len = (size_t)written;
        if (len >= sizeof(buf)) {
            /* Truncated: keep what fits, replace the tail with a marker so a partial
             * line cannot be mistaken for a whole one. */
            len = sizeof(buf) - 1;
            if (len > B220_TCP_LOG_CONT_LEN) {
                memcpy(&buf[len - B220_TCP_LOG_CONT_LEN], B220_TCP_LOG_CONT_MARK,
                       B220_TCP_LOG_CONT_LEN);
            }
        }
        if (len > B220_TCP_LOG_MAX_MSG) {
            len = B220_TCP_LOG_MAX_MSG;
        }

        if (xSemaphoreTake(ring_mutex_, portMAX_DELAY) != pdTRUE) {
            return;
        }

        /* Make room, dropping the oldest data rather than lapping a client. */
        const size_t used = (write_pos_ + B220_TCP_LOG_RING - oldest_pos_) % B220_TCP_LOG_RING;
        if (used + len > B220_TCP_LOG_RING) {
            const size_t need = used + len - B220_TCP_LOG_RING;
            oldest_pos_ = (oldest_pos_ + need) % B220_TCP_LOG_RING;
            for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
                if (client_fd_[i] >= 0) {
                    read_pos_[i] = oldest_pos_;
                }
            }
        }

        /* Copy, splitting at the end of the ring.  Only the data bytes are copied: a
         * NUL in the middle of the stream would look like corruption to a client. */
        const size_t to_end = B220_TCP_LOG_RING - write_pos_;
        if (len <= to_end) {
            memcpy(&ring_[write_pos_], buf, len);
        } else {
            memcpy(&ring_[write_pos_], buf, to_end);
            memcpy(ring_, buf + to_end, len - to_end);
        }
        write_pos_ = (write_pos_ + len) % B220_TCP_LOG_RING;

        /* Any client that the write just lapped jumps to the oldest surviving byte. */
        for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
            if (client_fd_[i] >= 0 && read_pos_[i] == write_pos_) {
                read_pos_[i] = oldest_pos_;
            }
        }

        xSemaphoreGive(ring_mutex_);
    }

    /* ------------------------------------------------------------ ring buffer */

    void append(const char* data, size_t len)
    {
        if (len == 0 || len > B220_TCP_LOG_RING) {
            return;
        }
        if (xSemaphoreTake(ring_mutex_, portMAX_DELAY) != pdTRUE) {
            return;
        }

        /* A chunk that does not fit would lap the clients, so drop the oldest data
         * first.  The line has already gone to serial, so losing very old lines is the
         * right trade. */
        const size_t used = (write_pos_ + B220_TCP_LOG_RING - oldest_pos_) % B220_TCP_LOG_RING;
        if (used + len > B220_TCP_LOG_RING) {
            const size_t need = used + len - B220_TCP_LOG_RING;
            oldest_pos_ = (oldest_pos_ + need) % B220_TCP_LOG_RING;
            for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
                if (client_fd_[i] >= 0) {
                    read_pos_[i] = oldest_pos_;
                }
            }
        }

        for (size_t i = 0; i < len; i++) {
            ring_[write_pos_] = data[i];
            write_pos_ = (write_pos_ + 1) % B220_TCP_LOG_RING;
        }

        xSemaphoreGive(ring_mutex_);
    }

    /* -------------------------------------------------------------- listener */

    static void task_entry(void* arg) { static_cast<TcpLog*>(arg)->run(); }

    void run()
    {
        /*
         * WAIT FOR LWIP BEFORE TOUCHING A SOCKET.
         *
         * The static initialiser installs the log hook, which has to happen before
         * anything logs - but it runs far too early to open a socket.  On this firmware
         * esp_netif_init()/board.StartNetwork() happen inside Application::Initialize(),
         * i.e. after the board constructor and long after static initialisation, and
         * doing it in the initialiser asserted inside lwIP:
         *
         *     assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
         *
         * So the split is: the RING BUFFER and the HOOK start at static-init time (they
         * need nothing but RAM, and they are what captures the boot log), while the
         * LISTENER waits here until the network stack exists.  A task context is also
         * the right place for blocking bind/listen/accept.
         */
        if (!wait_for_network()) {
            /* No network stack ever appeared.  The hook, the ring and the serial dump
             * fallback all still work, so this is a degraded mode, not a failure. */
            vTaskDelete(nullptr);
            return;
        }

        const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0) {
            return;
        }
        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(B220_TCP_LOG_PORT);
        bool bound = false;
        for (int attempt = 0; attempt < 20 && !bound; attempt++) {
            if (bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0 &&
                listen(listener, B220_TCP_LOG_MAX_CLIENTS) == 0) {
                bound = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!bound) {
            close(listener);
            /* The ring and the hook stay alive, so the serial dump still works. */
            vTaskDelete(nullptr);
            return;
        }

        for (;;) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listener, &read_fds);
            int max_fd = listener;
            for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
                if (client_fd_[i] >= 0) {
                    FD_SET(client_fd_[i], &read_fds);
                    if (client_fd_[i] > max_fd) {
                        max_fd = client_fd_[i];
                    }
                }
            }

            struct timeval tv = {};
            tv.tv_sec = 0;
            tv.tv_usec = B220_TCP_LOG_POLL_MS * 1000;
            const int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
            if (ready > 0) {
                if (FD_ISSET(listener, &read_fds)) {
                    accept_client(listener);
                }
                /*
                 * A readable client either hung up (drop it) or sent something, which on
                 * this device is a COMMAND: the same {BASE+...} lines the serial test
                 * channel accepts.  The line is accumulated per client, so a client that
                 * types in pieces still gets its line parsed once.
                 */
                for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
                    if (client_fd_[i] < 0 || !FD_ISSET(client_fd_[i], &read_fds)) {
                        continue;
                    }
                    uint8_t chunk[64];
                    const ssize_t got = recv(client_fd_[i], chunk, sizeof(chunk), MSG_DONTWAIT);
                    if (got <= 0) {
                        if (got < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                            continue;   /* nothing to read after all */
                        }
                        drop_client(i);
                        continue;
                    }
                    for (ssize_t b = 0; b < got; b++) {
                        const char c = (char)chunk[b];
                        if (c == '\r' || c == '\n' || c == '}') {
                            if (c == '}') {
                                /* Keep the brace: the UART parser expects it. */
                                if (client_line_len_[i] < sizeof(client_line_[i]) - 1) {
                                    client_line_[i][client_line_len_[i]++] = c;
                                }
                            }
                            client_line_[i][client_line_len_[i]] = '\0';
                            if (client_line_len_[i] > 0) {
                                /*
                                 * Hand it to the same parser the serial channel uses.
                                 *
                                 * DELIBERATELY NOT LOGGED HERE.  This file has no TAG and
                                 * does not log at all: it IS the log, so a message from
                                 * inside it would go through the hook that is calling this
                                 * code.  The acknowledgement is the parser's own output
                                 * ("test: MOVE ...", "SWEEP 1/8: ..."), which reaches the
                                 * client through the normal stream a moment later.
                                 */
                                b220_base_uart_handle_test_command(client_line_[i]);
                            }
                            client_line_len_[i] = 0;
                        } else if (client_line_len_[i] < sizeof(client_line_[i]) - 1) {
                            client_line_[i][client_line_len_[i]++] = c;
                        } else {
                            /* Longer than any command: drop the partial line rather than
                             * keep growing it. */
                            client_line_len_[i] = 0;
                        }
                    }
                }
            }
            pump();
        }
    }

    /* Poll until the default netif exists, which only happens once esp_netif_init() has
     * run and the network has been started.  Polling costs one cheap check per second
     * and removes any need to know this firmware's init order - which is exactly the
     * assumption that was wrong before. */
    bool wait_for_network()
    {
        for (int attempt = 0; attempt < 60; attempt++) {
            if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr ||
                esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") != nullptr) {
                /* Let the stack settle a moment before the first socket. */
                vTaskDelay(pdMS_TO_TICKS(200));
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        return false;
    }

    void accept_client(int listener)
    {
        struct sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(listener, reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            return;
        }
        int slot = -1;
        for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
            if (client_fd_[i] < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            const char busy[] = "[tcplog] too many clients\r\n";
            (void)send(fd, busy, sizeof(busy) - 1, 0);
            close(fd);
            return;
        }
        int yes = 1;
        /* Keepalive, so a client that vanished (laptop slept, cable pulled) is
         * eventually detected instead of holding a slot forever. */
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
        client_fd_[slot] = fd;
        /* Start from the oldest byte still in the ring rather than from "now", so a
         * client that connects after WiFi came up still sees the boot log. */
        read_pos_[slot] = oldest_pos_;
        client_count_++;
        /* One line the client sees immediately even when the device is idle, so
         * "connected but quiet" is distinguishable from "never connected". */
        const char hello[] =
            "\r\n[tcplog] attached: buffered history follows.\r\n"
            "[tcplog] You can also TYPE COMMANDS here, one per line:\r\n"
            "[tcplog]   {BASE+MOVE,forward,100,3000}   {BASE+STOP}   {BASE+LIGHT,blue}\r\n"
            "[tcplog]   {BASE+SWEEP}   (walks the motion parameters, logging each step)\r\n";
        (void)send(fd, hello, sizeof(hello) - 1, 0);
    }

    void drop_client(int slot)
    {
        if (client_fd_[slot] < 0) {
            return;
        }
        close(client_fd_[slot]);
        client_fd_[slot] = -1;
        read_pos_[slot] = 0;
        if (client_count_ > 0) {
            client_count_--;
        }
    }

    /* Push whatever is new to each client. */
    void pump()
    {
        if (xSemaphoreTake(ring_mutex_, portMAX_DELAY) != pdTRUE) {
            return;
        }
        for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
            if (client_fd_[i] < 0) {
                continue;
            }
            while (read_pos_[i] != write_pos_) {
                /* Send a contiguous run, so a burst is a few sends rather than one per
                 * byte. */
                size_t run = (read_pos_[i] < write_pos_)
                                 ? (write_pos_ - read_pos_[i])
                                 : (B220_TCP_LOG_RING - read_pos_[i]);
                if (run > 512) {
                    run = 512;
                }
                const ssize_t sent =
                    send(client_fd_[i], &ring_[read_pos_[i]], run, MSG_DONTWAIT);
                if (sent <= 0) {
                    /* Would block on a slow client: leave the rest for the next pass.
                     * A real error drops the client. */
                    if (sent < 0 && errno != EWOULDBLOCK && errno != EAGAIN &&
                        errno != ENOBUFS) {
                        drop_client(i);
                    }
                    break;
                }
                read_pos_[i] = (read_pos_[i] + (size_t)sent) % B220_TCP_LOG_RING;
            }
        }
        xSemaphoreGive(ring_mutex_);
    }

    /* --------------------------------------------------------------- storage */

    vprintf_like_t previous_ = nullptr;
    SemaphoreHandle_t ring_mutex_ = nullptr;
    bool started_ = false;
    bool starting_ = false;

    char ring_[B220_TCP_LOG_RING] = {};
    size_t write_pos_ = 0;
    size_t oldest_pos_ = 0;

    int client_fd_[B220_TCP_LOG_MAX_CLIENTS] = {-1, -1, -1, -1};
    size_t read_pos_[B220_TCP_LOG_MAX_CLIENTS] = {};
    /* One command line being typed per client.  96 bytes is well past the longest command
     * this device accepts. */
    char client_line_[B220_TCP_LOG_MAX_CLIENTS][96] = {};
    size_t client_line_len_[B220_TCP_LOG_MAX_CLIENTS] = {};
    int client_count_ = 0;
};

/*
 * Installed before app_main() by the C++ runtime.  A static initialiser is the earliest
 * point this board can run code (the board constructor is much later), which is why the
 * TCP stream contains the whole boot from the board's first log line on.
 *
 * ONLY the hook and the ring buffer may run this early.  Anything that touches lwIP has
 * to wait for the network stack - see the wait at the top of TcpLog::run().
 */
const int kTcpLogInstalled = (TcpLog::instance().start(), 0);

}  // namespace

/* C linkage forwarders, so the header stays C-compatible. */
extern "C" esp_err_t b220_tcp_log_start(void)
{
    TcpLog::instance().start();
    return ESP_OK;
}

extern "C" void b220_tcp_log_dump(void)
{
    TcpLog::instance().dump();
}

extern "C" int b220_tcp_log_clients(void)
{
    return TcpLog::instance().clients();
}
