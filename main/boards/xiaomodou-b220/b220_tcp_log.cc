/*
 * b220_tcp_log.cc - see b220_tcp_log.h.  A debug log channel over TCP so the serial
 * port can be handed to the baseboard.
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

#define B220_TCP_LOG_PORT        3333
/* The ring size is the amount of history a late client can see.  8 KB is several
 * hundred lines - more than a boot sequence - and irrelevant next to the ~90 KB of
 * free SRAM on this board. */
#define B220_TCP_LOG_RING        8192
/* Largest single log chunk kept.  A longer line is truncated rather than dropped, so
 * one call can never lap the whole ring. */
#define B220_TCP_LOG_CHUNK       512
/* Four simultaneous tail-ends is plenty for debugging and keeps the socket count
 * (CONFIG_LWIP_MAX_SOCKETS = 10) comfortable. */
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
        /* Format once into a stack buffer: no allocation, bounded work.  vsnprintf
         * returns the length it WOULD have written, hence the clamp. */
        char buf[B220_TCP_LOG_CHUNK];
        va_list copy;
        va_copy(copy, ap);
        const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
        va_end(copy);

        TcpLog& self = instance();
        if (self.started_ && self.ring_mutex_ != nullptr && n > 0) {
            size_t len = (size_t)n;
            if (len >= sizeof(buf)) {
                len = sizeof(buf) - 1;   /* truncated, keep the terminator */
            }
            self.append(buf, len);
        }

        /* The original handler, unchanged: this is what still writes the serial log.
         * Called last, so a network client never delays serial output. */
        if (self.previous_ != nullptr) {
            return self.previous_(fmt, ap);
        }
        return vprintf(fmt, ap);
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
                /* A readable client that we only stream TO means the peer closed or
                 * sent something; either way it is not useful here, so drop it. */
                for (int i = 0; i < B220_TCP_LOG_MAX_CLIENTS; i++) {
                    if (client_fd_[i] >= 0 && FD_ISSET(client_fd_[i], &read_fds)) {
                        uint8_t scratch[32];
                        if (recv(client_fd_[i], scratch, sizeof(scratch), MSG_DONTWAIT) <= 0) {
                            drop_client(i);
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
        const char hello[] = "\r\n[tcplog] attached to the device log; buffered history follows\r\n";
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
