#include <liburing.h>
#include <liburing/io_uring.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace net {
namespace xiouring {

static constexpr int32_t kQueueDepth = 1;

namespace sys {

int32_t IOURingSetup(uint32_t entries, io_uring_params *p) {
  return (int32_t)syscall(__NR_io_uring_setup, entries, p);
}

int32_t IOURingEnter(int32_t ring_fd, uint32_t to_submit,
                     uint32_t minimum_complete, uint32_t flags) {
  return (int32_t)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                          minimum_complete, flags, NULL, 0);
}

namespace files {

static constexpr uint64_t kBlockSize = 1024;

struct FileInfo {
  uint64_t file_size_;
  iovec iovectors[];
};

int64_t GetFileSize(int32_t fd) noexcept {
  struct stat st;
  if (fstat(fd, &st) < 0) {
    std::cerr << "fstat failed on fd " << fd << '\n';
    return -1;
  }
  if (S_ISBLK(st.st_mode)) {
    int64_t byte_count;
    if (ioctl(fd, BLKGETSIZE64, &byte_count) != 0) {
      std::cerr << "ioctl BLKGETSIZE64 failed on fd " << fd << '\n';
      return -1;
    }
    return byte_count;
  } else if (S_ISREG(st.st_mode)) {
    return st.st_size;
  }
  return -1;
}

}  // namespace files

namespace io {

void PrintNChar(char *buffer, int32_t len) {
  while (len--) std::cout << *buffer++;
}

}  // namespace io

}  // namespace sys

struct IOSubmissionQueueRing {
  uint32_t *head_;
  uint32_t *tail_;
  uint32_t *ring_mask_;
  uint32_t *ring_entries_;
  uint32_t *flags_;
  uint32_t *array_;
};

struct IOCompletionQueueRing {
  uint32_t *head_;
  uint32_t *tail_;
  uint32_t *ring_mask_;
  uint32_t *ring_entries_;
  io_uring_cqe *completion_queue_events_;
};

struct Submitter {
 public:
  int32_t ring_fd_;
  IOSubmissionQueueRing sq_ring_;
  io_uring_sqe *submission_queue_events;
  IOCompletionQueueRing cq_ring_;

  int32_t SetupURing() noexcept {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    ring_fd_ = sys::IOURingSetup(kQueueDepth, &p);
    if (ring_fd_ < 0) {
      std::cerr << "IOURingSetup failed\n";
      return 1;
    }
    int32_t submission_ring_size =
        p.sq_off.array + p.sq_entries * sizeof(unsigned);
    int32_t completion_ring_size =
        p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
      if (completion_ring_size > submission_ring_size)
        submission_ring_size = completion_ring_size;
      completion_ring_size = submission_ring_size;
    }
    char *submission_queue_ptr, *completion_queue_ptr;
    submission_queue_ptr =
        (char *)mmap(0, submission_ring_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
    if (submission_queue_ptr == MAP_FAILED) {
      std::cerr << "mmap failed to allocate submission queue memory\n";
      return 1;
    }
    if (p.features & IORING_FEAT_SINGLE_MMAP)
      completion_queue_ptr = submission_queue_ptr;
    else {
      completion_queue_ptr =
          (char *)mmap(0, completion_ring_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
      if (completion_queue_ptr == MAP_FAILED) {
        std::cerr << "mmap failed to allocate completion queue memory\n";
        return 1;
      }
    }
    sq_ring_.head_ = (uint32_t *)submission_queue_ptr + p.sq_off.head;
    sq_ring_.tail_ = (uint32_t *)submission_queue_ptr + p.sq_off.tail;
    sq_ring_.ring_mask_ = (uint32_t *)submission_queue_ptr + p.sq_off.ring_mask;
    sq_ring_.ring_entries_ =
        (uint32_t *)submission_queue_ptr + p.sq_off.ring_entries;
    sq_ring_.flags_ = (uint32_t *)submission_queue_ptr + p.sq_off.flags;
    sq_ring_.array_ = (uint32_t *)submission_queue_ptr + p.sq_off.array;
    submission_queue_events = (io_uring_sqe *)mmap(
        0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES);
    if (submission_queue_events == MAP_FAILED) {
      std::cerr << "mmap failed to allocate submission queue event buffer\n";
      return 1;
    }
    cq_ring_.head_ = (uint32_t *)completion_queue_ptr + p.cq_off.head;
    cq_ring_.tail_ = (uint32_t *)completion_queue_ptr + p.cq_off.tail;
    cq_ring_.ring_mask_ = (uint32_t *)completion_queue_ptr + p.cq_off.ring_mask;
    cq_ring_.ring_entries_ =
        (uint32_t *)completion_queue_ptr + p.cq_off.ring_entries;
    cq_ring_.completion_queue_events_ =
        (io_uring_cqe *)completion_queue_ptr + p.cq_off.cqes;
    return 0;
  }

  int32_t SubmitCat(char *file_path) noexcept {
    if (!file_path) {
      std::cerr << "file_path is empty\n";
      return 1;
    }
    sys::files::FileInfo *file_info;
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
      std::cerr << "Failed to open file for cat: " << file_path << '\n';
      return 1;
    }
    uint32_t index = 0, current_block = 0, tail = 0, next_tail = 0;
    int64_t file_size = sys::files::GetFileSize(file_fd);
    if (file_size < 0) {
      std::cerr << "GetFileSize(" << file_fd << ") returned negative value\n";
      return 1;
    }
    int64_t bytes_remaining = file_size;
    uint64_t block_count = (uint64_t)file_size / sys::files::kBlockSize;
    if (file_size % sys::files::kBlockSize) block_count++;
    file_info = (sys::files::FileInfo *)malloc(
        sizeof(sys::files::FileInfo) + sizeof(struct iovec) * block_count);
    if (!file_info) {
      std::cerr
          << "Failed to allocate memory for submission file_info struct\n";
      return 1;
    }
    file_info->file_size_ = file_size;
    while (bytes_remaining) {
      uint64_t bytes_to_read = bytes_remaining;
      if (bytes_to_read > sys::files::kBlockSize)
        bytes_to_read = sys::files::kBlockSize;
      file_info->iovectors[current_block].iov_len = bytes_to_read;
      char *buffer;
      if (posix_memalign((void **)&buffer, sys::files::kBlockSize,
                         sys::files::kBlockSize)) {
        std::cerr << "Failed to align buffer to file block size\n";
        return 1;
      }
      file_info->iovectors[current_block].iov_base = buffer;
      current_block++;
      bytes_remaining -= bytes_to_read;
    }
    next_tail = tail = *sq_ring_.tail_;
    next_tail++;
    __asm__ __volatile__("" ::: "memory");
    index = tail & *sq_ring_.ring_mask_;
    io_uring_sqe *sqe = &submission_queue_events[index];
    sqe->fd = file_fd;
    sqe->flags = 0;
    sqe->opcode = IORING_OP_READV;
    sqe->addr = (uint64_t)file_info->iovectors;
    sqe->len = block_count;
    sqe->off = 0;
    sqe->user_data = (uint64_t)file_info;
    sq_ring_.array_[index] = index;
    tail = next_tail;
    if (*sq_ring_.tail_ != tail) {
      *sq_ring_.tail_ = tail;
      __asm__ __volatile__("" ::: "memory");
    }
    int status = sys::IOURingEnter(ring_fd_, 1, 1, IORING_ENTER_GETEVENTS);
    if (status < 0) {
      std::cerr << "iouringenter failed\n";
      return 1;
    }
    return 0;
  }

  void ReadFromCompletionQueue() noexcept {
    sys::files::FileInfo *file_info;
    struct io_uring_cqe *cqe;
    uint32_t head, reaped = 0;
    head = *cq_ring_.head_;
    do {
      __asm__ __volatile__("" ::: "memory");
      if (head == *cq_ring_.tail_) continue;
      cqe = &cq_ring_.completion_queue_events_[head & *cq_ring_.ring_mask_];
      file_info = (sys::files::FileInfo *)cqe->user_data;
      uint64_t block_count =
          (uint64_t)file_info->file_size_ / sys::files::kBlockSize;
      if (file_info->file_size_ % sys::files::kBlockSize) block_count++;
      for (uint64_t i = 0; i < block_count; ++i)
        sys::io::PrintNChar((char *)file_info->iovectors[i].iov_base,
                            file_info->iovectors[i].iov_len);
      head++;
    } while (1);
    *cq_ring_.head_ = head;
    __asm__ __volatile__("" ::: "memory");
  }

 private:
  std::atomic<bool> fence_{false};
};

}  // namespace xiouring
}  // namespace net

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <filenames>\n";
    return 1;
  }
  net::xiouring::Submitter *s;
  s = (net::xiouring::Submitter *)malloc(sizeof(*s));
  if (!s) {
    std::cerr << "Failed to allocate memory for submitter\n";
    return 1;
  }

  if (s->SetupURing()) {
    std::cerr << "SetupURing failed\n";
    return 1;
  }
  for (int i = 1; i < argc; ++i) {
    if (s->SubmitCat(argv[i])) {
      std::cerr << "Failed to cat " << argv[i];
    }
    s->ReadFromCompletionQueue();
  }
  return 0;
}
