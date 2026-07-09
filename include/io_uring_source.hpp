#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <liburing.h>

#include "source.hpp"

struct IoUringSource : ByteSource {
    static constexpr int    QD  = 4;
    static constexpr size_t BUF = 1u << 20;

    io_uring ring{};
    int      fd;
    size_t   file_size;
    uint64_t total_blocks;

    std::vector<uint8_t> mem;
    uint8_t* bufs[QD];
    iovec    iov[QD];

    struct Slot { bool ready = false; ssize_t res = 0; uint64_t seq = ~0ull; };
    Slot slot[QD];

    uint64_t next_to_return = 0;
    uint64_t next_to_submit = 0;
    int      pending_free   = -1;
    bool     ok             = true;

    IoUringSource(int fd_, size_t sz) : fd(fd_), file_size(sz) {
        total_blocks = (file_size + BUF - 1) / BUF;
        mem.resize((size_t)QD * BUF);
        for(int i = 0; i < QD; i++){ bufs[i] = mem.data() + (size_t)i * BUF; iov[i] = { bufs[i], BUF }; }

        if(io_uring_queue_init(QD, &ring, 0) < 0){ perror("io_uring_queue_init"); ok = false; return; }
        if(io_uring_register_buffers(&ring, iov, QD) < 0){ perror("register_buffers"); ok = false; return; }
        if(io_uring_register_files(&ring, &fd, 1) < 0){ perror("register_files"); ok = false; return; }

        for(int i = 0; i < QD && next_to_submit < total_blocks; i++)
            submit_block(next_to_submit++, i);
        io_uring_submit(&ring);
    }

    void submit_block(uint64_t seq, int slot_idx){
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        off_t  off  = (off_t)seq * BUF;
        size_t want = std::min(BUF, file_size - (size_t)off);
        io_uring_prep_read_fixed(sqe, 0 /*fixed file idx*/, bufs[slot_idx], want, off, slot_idx);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, seq);
    }

    const uint8_t* next(size_t& len) override {
        if(!ok || next_to_return >= total_blocks) return nullptr;

        if(pending_free >= 0 && next_to_submit < total_blocks){
            submit_block(next_to_submit++, pending_free);
            io_uring_submit(&ring);
            pending_free = -1;
        }

        int want_slot = next_to_return % QD;
        while(!(slot[want_slot].ready && slot[want_slot].seq == next_to_return)){
            io_uring_cqe* cqe;
            if(io_uring_wait_cqe(&ring, &cqe) < 0){ perror("wait_cqe"); ok = false; return nullptr; }
            uint64_t seq = io_uring_cqe_get_data64(cqe);
            int s = (int)(seq % QD);
            slot[s].ready = true; slot[s].res = cqe->res; slot[s].seq = seq;
            io_uring_cqe_seen(&ring, cqe);
        }

        if(slot[want_slot].res < 0){ fprintf(stderr, "read err %zd\n", slot[want_slot].res); ok = false; return nullptr; }
        len = (size_t)slot[want_slot].res;
        slot[want_slot].ready = false;
        pending_free = want_slot;
        next_to_return++;
        return bufs[want_slot];
    }

    ~IoUringSource(){ if(ok) io_uring_queue_exit(&ring); }
};