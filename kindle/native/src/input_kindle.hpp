#pragma once

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>

namespace kindle {

enum class EventType {
    NONE = 0,
    TOUCH_DOWN,
    TOUCH_MOVE,
    TOUCH_UP,
    KEY_PREV_PAGE,
    KEY_NEXT_PAGE
};

struct InputEvent {
    EventType type = EventType::NONE;
    int x = 0;
    int y = 0;
};

class InputKindle {
private:
    int touch_fd_ = -1;
    int button_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex queue_mutex_;
    std::queue<InputEvent> event_queue_;

public:
    InputKindle() = default;
    ~InputKindle() { stop(); }

    bool start(const char* touch_dev = "/dev/input/event9", const char* button_dev = "/dev/input/event4") {
        touch_fd_ = open(touch_dev, O_RDONLY | O_NONBLOCK);
        if (touch_fd_ < 0) {
            // Fallback: search for touchscreen event
            for (int i = 0; i <= 10; ++i) {
                char path[32];
                snprintf(path, sizeof(path), "/dev/input/event%d", i);
                int fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    char name[128] = {0};
                    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                    if (strstr(name, "cyttsp") || strstr(name, "touch")) {
                        touch_fd_ = fd;
                        break;
                    }
                    close(fd);
                }
            }
        }

        button_fd_ = open(button_dev, O_RDONLY | O_NONBLOCK);
        if (button_fd_ < 0) {
            // Fallback for button device
            for (int i = 0; i <= 10; ++i) {
                char path[32];
                snprintf(path, sizeof(path), "/dev/input/event%d", i);
                int fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    char name[128] = {0};
                    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                    if (strstr(name, "gpio-keys") || strstr(name, "keys")) {
                        button_fd_ = fd;
                        break;
                    }
                    close(fd);
                }
            }
        }

        printf("InputKindle: touch_fd=%d, button_fd=%d\n", touch_fd_, button_fd_);
        if (touch_fd_ < 0 && button_fd_ < 0) {
            return false;
        }

        running_ = true;
        thread_ = std::thread(&InputKindle::readerLoop, this);
        return true;
    }

    void stop() {
        if (running_) {
            running_ = false;
            if (thread_.joinable()) {
                thread_.join();
            }
        }
        if (touch_fd_ >= 0) { close(touch_fd_); touch_fd_ = -1; }
        if (button_fd_ >= 0) { close(button_fd_); button_fd_ = -1; }
    }

    bool pollEvent(InputEvent& out) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (event_queue_.empty()) return false;
        out = event_queue_.front();
        event_queue_.pop();
        return true;
    }

    // Drop taps that queued up while the UI was blocked (e.g. during network calls)
    void clear() {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::queue<InputEvent>().swap(event_queue_);
    }

private:
    void readerLoop() {
        struct pollfd fds[2];
        int num_fds = 0;
        int touch_idx = -1;
        int btn_idx = -1;

        if (touch_fd_ >= 0) {
            touch_idx = num_fds;
            fds[num_fds].fd = touch_fd_;
            fds[num_fds].events = POLLIN;
            num_fds++;
        }
        if (button_fd_ >= 0) {
            btn_idx = num_fds;
            fds[num_fds].fd = button_fd_;
            fds[num_fds].events = POLLIN;
            num_fds++;
        }

        int cur_x = 0;
        int cur_y = 0;
        bool is_down = false;

        while (running_) {
            int ret = poll(fds, num_fds, 50);
            if (ret <= 0) continue;

            // 1. Process Touchscreen
            if (touch_idx >= 0 && (fds[touch_idx].revents & POLLIN)) {
                struct input_event ev[32];
                int rd = read(touch_fd_, ev, sizeof(ev));
                int count = rd / sizeof(struct input_event);

                for (int i = 0; i < count; ++i) {
                    if (ev[i].type == EV_ABS) {
                        if (ev[i].code == ABS_MT_POSITION_X || ev[i].code == ABS_X) {
                            cur_x = ev[i].value;
                        } else if (ev[i].code == ABS_MT_POSITION_Y || ev[i].code == ABS_Y) {
                            cur_y = ev[i].value;
                        } else if (ev[i].code == ABS_MT_TRACKING_ID) {
                            if (ev[i].value >= 0 && !is_down) {
                                is_down = true;
                            } else if (ev[i].value < 0 && is_down) {
                                is_down = false;
                                pushEvent({EventType::TOUCH_UP, cur_x, cur_y});
                            }
                        }
                    } else if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH) {
                        if (ev[i].value == 1 && !is_down) {
                            is_down = true;
                        } else if (ev[i].value == 0 && is_down) {
                            is_down = false;
                            pushEvent({EventType::TOUCH_UP, cur_x, cur_y});
                        }
                    }
                }
            }

            // 2. Process Physical Page-Turn Buttons
            if (btn_idx >= 0 && (fds[btn_idx].revents & POLLIN)) {
                struct input_event ev[16];
                int rd = read(button_fd_, ev, sizeof(ev));
                int count = rd / sizeof(struct input_event);

                for (int i = 0; i < count; ++i) {
                    if (ev[i].type == EV_KEY && ev[i].value == 1) { // Key down
                        if (ev[i].code == KEY_PAGEUP || ev[i].code == 104) {
                            pushEvent({EventType::KEY_PREV_PAGE, 0, 0});
                        } else if (ev[i].code == KEY_PAGEDOWN || ev[i].code == 109) {
                            pushEvent({EventType::KEY_NEXT_PAGE, 0, 0});
                        }
                    }
                }
            }
        }
    }

    void pushEvent(const InputEvent& ev) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // If queue has pending events, drop oldest so freshest touch events are always handled instantly
        if (event_queue_.size() >= 16) {
            event_queue_.pop();
        }
        event_queue_.push(ev);
    }
};

} // namespace kindle
