/*
 * Defuse to Wake - fixed-capacity ring buffer (pure, no heap, trivially copyable so the whole
 * object can be stored in flash as one blob). push() on a full ring drops the OLDEST item.
 * Not thread-safe: each instance is owned by one task.
 */
#ifndef FIXED_RING_H
#define FIXED_RING_H

#include <stdint.h>
#include <string.h>

template <typename T, int N>
struct FixedRing {
    T items[N];
    uint8_t head;    /* slot of the oldest item */
    uint8_t count;

    void clear() { memset(static_cast<void *>(this), 0, sizeof(*this)); }

    int size() const { return count; }

    /* True if the ring was full and the oldest item was discarded to make room. */
    bool push(const T &v)
    {
        bool dropped = false;

        if (count == N) {
            head = (uint8_t)((head + 1) % N);
            count--;
            dropped = true;
        }
        items[(head + count) % N] = v;
        count++;
        return dropped;
    }

    /* i = 0 is the oldest item. Caller keeps 0 <= i < size(). */
    const T &oldest(int i) const { return items[(head + i) % N]; }

    /* i = 0 is the newest item. Caller keeps 0 <= i < size(). */
    const T &newest(int i) const { return items[(head + count - 1 - i) % N]; }

    bool pop_oldest()
    {
        if (count == 0) {
            return false;
        }
        head = (uint8_t)((head + 1) % N);
        count--;
        return true;
    }

    /* Sanity check for a ring read back from flash. */
    bool valid() const { return head < N && count <= N; }
};

#endif /* FIXED_RING_H */
