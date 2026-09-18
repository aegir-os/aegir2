/*
 * Turning the manifest into processes -- implementation. See src/services.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "services.h"

namespace aegir::director {

namespace {

/** The manifest's priority, or the one just below ours: a supervisor that cannot
 *  preempt what it supervises is not supervising it. */
uint32_t priority_for(manifest::Entry const &entry) noexcept
{
    uint32_t value = 0;
    bool any = false;
    for (uint32_t i = 0; i < entry.priority.length; ++i) {
        char const digit = entry.priority.data[i];
        if (digit < '0' || digit > '9') {
            any = false;
            break;
        }
        value = value * 10 + static_cast<uint32_t>(digit - '0');
        any = true;
    }
    if (!any || value == 0 || value > seL4_MaxPrio) {
        return seL4_MaxPrio - 1;
    }
    return value;
}

bool declared_as_user(manifest::Entry const &entry) noexcept
{
    return entry.authority == manifest::Authority::User;
}

/** Does a `spawns` item name this entry? An item is either an exact name or a
 *  class: `fs.*` covers every entry whose name begins `fs.` -- matched
 *  literally up to the star, which is the whole of the rule
 *  (specs/services.md). */
bool spawn_covers(manifest::View item, manifest::View name) noexcept
{
    uint32_t prefix = item.length;
    if (prefix > 0 && item.data[prefix - 1] == '*') {
        --prefix;
        if (name.length < prefix) {
            return false;
        }
    } else if (name.length != prefix) {
        return false;
    }
    for (uint32_t k = 0; k < prefix; ++k) {
        if (name.data[k] != item.data[k]) {
            return false;
        }
    }
    return true;
}

/** Call `each` with every item of a comma-separated `spawns` value, trimmed --
 *  the same shape ports.cc's split_names gives `owns` and `needs`, kept local
 *  because a spawn right is this file's business, not the port graph's. */
template <typename F>
void for_each_spawn(manifest::View spawns, F &&each) noexcept
{
    uint32_t at = 0;
    while (at < spawns.length) {
        while (at < spawns.length && (spawns.data[at] == ',' || spawns.data[at] == ' ')) {
            ++at;
        }
        uint32_t end = at;
        while (end < spawns.length && spawns.data[end] != ',') {
            ++end;
        }
        uint32_t trimmed = end;
        while (trimmed > at && spawns.data[trimmed - 1] == ' ') {
            --trimmed;
        }
        if (trimmed > at) {
            each(manifest::View{spawns.data + at, trimmed - at});
        }
        at = end < spawns.length ? end + 1 : spawns.length;
    }
}

}  // namespace

Services::Services(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena,
                   spawn::Initrd const &initrd) noexcept
    : allocator_(allocator), arena_(arena), initrd_(initrd), fault_endpoint_(0),
      graph_(allocator, arena),
      /* Director's own pool is the boot set's: delegation hands a service a pool
       * of its own, so only director-spawned address spaces come from the
       * initial one (specs/authority.md). */
      spawner_(allocator, scratch, arena, initrd, seL4_CapInitThreadASIDPool,
               seL4_CapInitThreadCNode, seL4_WordBits)
{
}

bool Services::prepare(mem::Account &account) noexcept
{
    seL4_Error error = seL4_NoError;
    fault_endpoint_ = allocator_.alloc_object(seL4_EndpointObject, seL4_EndpointBits, account,
                                              &error);
    return fault_endpoint_ != 0;
}

void Services::boot(manifest::Manifest const &manifest, mem::Account &account, Started *started,
                    Boot &boot, Supervisor *supervisor, void const *devices,
              uint32_t devices_bytes, Device const *bus, uint32_t bus_count,
              spawn::PortGrant const *extra, uint32_t extra_count) noexcept
{
    boot.declared = manifest.size();
    boot.started = 0;
    boot.problem = "";

    /* Validate before creating anything (specs/services.md). The check that can
     * be made today is the one that matters most: nothing may name a binary the
     * image does not carry. */
    for (uint32_t i = 0; i < manifest.size(); ++i) {
        manifest::Entry const &entry = manifest[i];
        if (initrd_.find(entry.binary.data, entry.binary.length, nullptr) == nullptr) {
            boot.problem = "a service names a binary the initrd does not contain";
            return;
        }
        if (declared_as_user(entry)) {
            /* Nothing can be a user service at *boot*: there is no user
             * yet, and sessions are created after authentication
             * (specs/authority.md). One another entry spawns -- a session,
             * which auth starts when a login succeeds -- is not boot's to
             * start, and its entry is what drives the delegatable copies
             * its spawner is given (specs/services.md). */
            bool covered = false;
            for (uint32_t j = 0; j < manifest.size() && !covered; ++j) {
                for_each_spawn(manifest[j].spawns, [&](manifest::View item) {
                    if (spawn_covers(item, entry.name)) {
                        covered = true;
                    }
                });
            }
            if (!covered) {
                boot.problem = "a boot service cannot be declared as a user service";
                return;
            }
        }
        /* Every `spawns` item -- a name or a `prefix*` class -- must resolve to
         * at least one declared entry: a spawn right over nothing is a typo that
         * would only surface as a driver that never starts, so it is checked
         * here, where the whole set is at hand (specs/services.md). */
        bool unresolved_spawn = false;
        for_each_spawn(entry.spawns, [&](manifest::View item) {
            bool resolves = false;
            for (uint32_t j = 0; j < manifest.size() && !resolves; ++j) {
                resolves = spawn_covers(item, manifest[j].name);
            }
            if (!resolves) {
                unresolved_spawn = true;
            }
        });
        if (unresolved_spawn) {
            boot.problem = "a service spawns a name the manifest does not declare";
            return;
        }
    }

    /* The ports first: who owns what, who may call it, and the order that makes
     * an owner always run before its first consumer. Nothing is created until all
     * of it validates (src/ports.h). */
    if (!graph_.build(manifest, account)) {
        boot.problem = graph_.problem();
        return;
    }

    for (uint32_t step = 0; step < manifest.size(); ++step) {
        uint32_t const i = graph_.order()[step];
        manifest::Entry const &entry = manifest[i];
        /* A service another entry spawns is not director's to start: the device
         * manager starts its driver and its partition manager when it gets to
         * them (specs/services.md), and starting one twice is not a philosophical
         * problem -- the second copy would be given the device the first one
         * already drives. The check is transitive: a class one of *those* spawns
         * (`fs.*`) is skipped here too. */
        bool spawned_by_another = false;
        for (uint32_t j = 0; j < manifest.size() && !spawned_by_another; ++j) {
            for_each_spawn(manifest[j].spawns, [&](manifest::View item) {
                if (spawn_covers(item, entry.name)) {
                    spawned_by_another = true;
                }
            });
        }
        if (spawned_by_another) {
            continue;
        }
        spawn::Request request{};
        request.name = entry.name.data;
        request.name_length = entry.name.length;
        request.binary = entry.binary.data;
        request.binary_length = entry.binary.length;
        request.account = entry.account.data;
        request.account_length = entry.account.length;
        request.priority = priority_for(entry);
        spawn::PortGrant const *grants = graph_.grants(i);
        uint32_t grant_count = graph_.grant_count(i);
        /* Capabilities director delegates to a service go in beside the ports the
         * manifest declares. A pool is not a port and cannot come from the port
         * graph; it is installed the way a port is -- by name, which is how the child
         * finds it, and by slot, which is the layout's business and not the caller's
         * (specs/authority.md). The slots come after the service's own ports. */
        /* The memory the service asked for is carved, retyped into a page and mapped into the
         * child by the spawner -- the child is told the address it can write and the physical
         * base the device reads, and needs no authority of its own. The untyped stays behind:
         * a carved region that has been *split* cannot be given away (the kernel answers
         * `RevokeFirst`), which is what "a port could not be installed" turned out to mean. */
        uint64_t memory_physical = 0;
        uint32_t memory_bits = 0;
        seL4_CPtr memory_cap = 0;
        seL4_CPtr memory_frame = 0;
        if (entry.memory_kib > 0) {
            memory_bits = 10;
            while ((1u << (memory_bits - 10)) < entry.memory_kib) {
                ++memory_bits;
            }
            seL4_Error memory_error = seL4_NoError;
            memory_cap =
                allocator_.carve_untyped(memory_bits, account, &memory_error, &memory_physical);
            if (memory_cap == 0) {
                boot.problem = "no memory for a service that asked for some";
                return;
            }
            /* The authority is the untyped; what the spawner needs is a *frame* it can map
             * into the child, and the child needs to be told the address it landed at --
             * which is the untyped's physical base, since a page-sized untyped is one page
             * (specs/services.md). */
            /* As many pages as were granted, retyped one after another. The allocator hands
             * out consecutive slots (alloc_slot bumps a cursor), which is what lets the
             * spawner map them as `memory_frame + i`. The queue needs two: the used ring
             * sits a page after the rest of it. */
            uint32_t const pages = (1u << memory_bits) / 4096u;
            seL4_Error page_error = seL4_NoError;
            for (uint32_t i = 0; i < pages; ++i) {
                seL4_CPtr const frame = allocator_.carve_page(memory_cap, account, &page_error);
                if (frame == 0) {
                    boot.problem = "the memory a service asked for could not be turned into pages";
                    return;
                }
                 if (i == 0) {
                     memory_frame = frame;
                 }
             }
         }
         /* A service that spawns is delegated what spawning takes, whoever it is
          * (specs/services.md, specs/authority.md): an untyped its children's
          * objects come out of, and an ASID pool their address space ids come
          * from. The kernel makes a pool from an *untyped* rather than by
          * retyping (seL4_ARCH_ASIDControl_MakePool; sel4test does the same in
          * projects/sel4test/apps/sel4test-tests/src/tests/vspace.c:141), so
          * both are this service's to carve here. */
         bool const spawner = entry.spawns.length > 0;
         /* The delegation's size grew with what a spawner must hold: the device
          * manager's share is three drivers' images, queue memories and windows,
          * the partition manager's megabyte, and the filesystem image that
          * manager is handed -- 2 MiB ran out under the third driver ("no memory
          * for a frame" mapping the blob). It is a budget, not a capacity: the
          * day spawners' needs diverge, the manifest says who gets how much. */
         constexpr uint32_t kDelegatedUntypedBits = 22;
         seL4_CPtr spawn_untyped = 0;
         uint64_t spawn_untyped_physical = 0;
         seL4_CPtr spawn_pool = 0;
         if (spawner) {
             seL4_Error kit_error = seL4_NoError;
             spawn_untyped = allocator_.carve_untyped(kDelegatedUntypedBits, account,
                                                      &kit_error, &spawn_untyped_physical);
             if (spawn_untyped == 0) {
                 boot.problem = "no untyped memory to delegate to a spawning service";
                 return;
             }
             spawn_pool = allocator_.make_asid_pool(account, &kit_error);
             if (spawn_pool == 0) {
                 boot.problem = "no ASID pool for a spawning service";
                 return;
             }
         }
         if ((entry.device_manager && extra_count > 0) || memory_cap != 0 || spawner) {
             uint32_t added = (entry.device_manager && extra_count > 0 ? extra_count : 0) +
                              (spawner ? 2 : 0);
            /* A spawning service also gets an *unbadged* copy of every port its
             * children need to call: a badged endpoint cap cannot be minted again
             * (deriveCap refuses it -- that is what "a port could not be installed"
             * with seL4_IllegalOperation meant), so a service that hands a port on
             * must be given one it may badge itself. The name carries a "spawn:"
             * prefix, because which copy is which is not something the block should
             * make a reader guess. The union over every entry its `spawns` covers
             * -- names and classes -- with each port once: two children both
             * needing log.main share one delegatable copy. The count is the worst
             * case; duplicates are dropped at the fill, and `at` says how many
             * there really are. */
            uint32_t spawn_needs = 0;
            if (spawner) {
                for (uint32_t j = 0; j < manifest.size(); ++j) {
                    bool covered = false;
                    for_each_spawn(entry.spawns, [&](manifest::View item) {
                        if (spawn_covers(item, manifest[j].name)) {
                            covered = true;
                        }
                    });
                    if (!covered) {
                        continue;
                    }
                    for (uint32_t g = 0; g < graph_.grant_count(j); ++g) {
                        if (graph_.grants(j)[g].badge != 0) {
                            ++spawn_needs;
                        }
                    }
                }
            }
            auto *merged = static_cast<spawn::PortGrant *>(
                arena_.allocate(sizeof(spawn::PortGrant) * (grant_count + added + spawn_needs)));
            if (merged == nullptr) {
                boot.problem = "no room to merge what a service is given";
                return;
            }
            uint32_t at = 0;
            for (uint32_t g = 0; g < grant_count; ++g) {
                merged[at++] = grants[g];
            }
            if (entry.device_manager && extra_count > 0) {
                for (uint32_t g = 0; g < extra_count; ++g) {
                    merged[at] = extra[g];
                    merged[at].slot = bootstrap::kSlotFirstDeclared + at;
                    ++at;
                }
            }
            /* The spawn kit, by name, which is how the child finds it: the
             * untyped carries its size, because a service cannot ask the kernel
             * how large an untyped is (there is no invocation that reads it),
             * so the grant has to say (specs/authority.md). */
            if (spawner) {
                static char const kUntypedGrant[] = "untyped";
                merged[at] = spawn::PortGrant{kUntypedGrant, sizeof(kUntypedGrant) - 1,
                                              bootstrap::kSlotFirstDeclared + at,
                                              spawn_untyped, seL4_AllRights, 0,
                                              kDelegatedUntypedBits};
                ++at;
                static char const kPoolGrant[] = "asid-pool";
                merged[at] = spawn::PortGrant{kPoolGrant, sizeof(kPoolGrant) - 1,
                                              bootstrap::kSlotFirstDeclared + at,
                                              spawn_pool, seL4_AllRights, 0, 0};
                ++at;
            }
            if (spawn_needs > 0) {
                static char const kSpawnPrefix[] = "spawn:";
                for (uint32_t j = 0; j < manifest.size(); ++j) {
                    bool covered = false;
                    for_each_spawn(entry.spawns, [&](manifest::View item) {
                        if (spawn_covers(item, manifest[j].name)) {
                            covered = true;
                        }
                    });
                    if (!covered) {
                        continue;
                    }
                    for (uint32_t g = 0; g < graph_.grant_count(j); ++g) {
                        spawn::PortGrant const &need = graph_.grants(j)[g];
                        if (need.badge == 0) {
                            continue;
                        }
                        /* One delegatable copy per port, however many children need
                         * it: matching by name, because the endpoint is what makes
                         * two "log.main"s the same port. The spawn: copies were
                         * appended after the service's own grants, which is where
                         * the comparison starts. */
                        bool already = false;
                        for (uint32_t m = grant_count + added; !already && m < at; ++m) {
                            spawn::PortGrant const &given = merged[m];
                            if (given.name_length !=
                                sizeof(kSpawnPrefix) - 1 + need.name_length) {
                                continue;
                            }
                            bool same = true;
                            for (uint32_t c = 0; c < need.name_length; ++c) {
                                if (given.name[sizeof(kSpawnPrefix) - 1 + c] !=
                                    need.name[c]) {
                                    same = false;
                                    break;
                                }
                            }
                            already = same;
                        }
                        if (already) {
                            continue;
                        }
                        auto *named = static_cast<char *>(
                            arena_.allocate(sizeof(kSpawnPrefix) - 1 + need.name_length));
                        if (named == nullptr) {
                            boot.problem = "no room to name a delegatable port";
                            return;
                        }
                        for (uint32_t c = 0; c < sizeof(kSpawnPrefix) - 1; ++c) {
                            named[c] = kSpawnPrefix[c];
                        }
                        for (uint32_t c = 0; c < need.name_length; ++c) {
                            named[sizeof(kSpawnPrefix) - 1 + c] = need.name[c];
                        }
                        merged[at] = spawn::PortGrant{
                            named,
                            static_cast<uint32_t>(sizeof(kSpawnPrefix) - 1) + need.name_length,
                            bootstrap::kSlotFirstDeclared + at,
                            need.capability, need.rights, 0, 0};
                        ++at;
                    }
                }
            }
            grants = merged;
            grant_count = at;
        }
        request.ports = grants;
        request.port_count = grant_count;
        /* A device is a capability, and its description is only useful with it, so
         * both go to the service that declares itself the device manager and to no
         * one else. Handing the frame to every service is what made the first child
         * claim it and every later one be refused (specs/services.md). */
        request.devices = entry.device_manager ? devices : nullptr;
        request.devices_bytes = entry.device_manager ? devices_bytes : 0;
        /* The device this service is *for*: its section names a bus device id and the survey
         * found the device that answers to it. Naming an id the bus does not have gets no
         * device, which the boot report shows -- a service given nothing is easier to see
         * than one given the wrong thing (specs/services.md). */
        Device const *mine = nullptr;
        if (entry.device_id != 0) {
            for (uint32_t d = 0; d < bus_count; ++d) {
                if (bus[d].id == entry.device_id) {
                    mine = &bus[d];
                    break;
                }
            }
        }
        request.device_frame = mine != nullptr ? mine->frame : 0;
        request.device_bytes = mine != nullptr ? 4096u : 0;
        request.device_physical = mine != nullptr ? mine->address : 0;
        request.untyped_physical = memory_physical;
        request.untyped_bits = memory_bits;
        /* A delegated untyped says where it is the same way (specs/authority.md):
         * the block's `untyped` entry is how the service learns both the size and
         * the physical base of the memory its objects come from. A spawner's
         * memory *is* its delegated untyped -- the carve above -- so the entry
         * describes it rather than any memory grant. */
        if (spawner) {
            request.untyped_physical = spawn_untyped_physical;
            request.untyped_bits = kDelegatedUntypedBits;
        }
        /* A service that spawns is given what spawning takes (specs/services.md,
         * specs/authority.md): its own VSpace root (the spawner grants a window of
         * free addresses with it), a copy of the initrd to read images out of, and
         * the devices its children are for -- as *capabilities* rather than
         * mappings, because a device manager's job is to hand them on. */
        if (spawner) {
            request.give_vspace = true;
            request.binaries = initrd_.blob();
            request.binaries_bytes = static_cast<uint32_t>(initrd_.blob_size());
            /* One device grant per covered child that is *for* a device, however
             * many of them the manifest declares: count first, so the array is
             * the manifest's size rather than a number somebody picked. */
            uint32_t wanted = 0;
            for (uint32_t j = 0; j < manifest.size(); ++j) {
                bool covered = false;
                for_each_spawn(entry.spawns, [&](manifest::View item) {
                    if (spawn_covers(item, manifest[j].name)) {
                        covered = true;
                    }
                });
                if (covered && manifest[j].device_id != 0) {
                    ++wanted;
                }
            }
            auto *device_grants = static_cast<spawn::DeviceGrant *>(
                arena_.allocate(sizeof(spawn::DeviceGrant) * (wanted != 0 ? wanted : 1)));
            if (device_grants == nullptr) {
                boot.problem = "no room to list what a spawning service is given";
                return;
            }
            uint32_t device_grant_count = 0;
            for (uint32_t j = 0; j < manifest.size(); ++j) {
                manifest::Entry const &child = manifest[j];
                bool covered = false;
                for_each_spawn(entry.spawns, [&](manifest::View item) {
                    if (spawn_covers(item, child.name)) {
                        covered = true;
                    }
                });
                if (!covered || child.device_id == 0) {
                    continue;
                }
                for (uint32_t d = 0; d < bus_count; ++d) {
                    if (bus[d].id == child.device_id) {
                        device_grants[device_grant_count++] =
                            spawn::DeviceGrant{bus[d].address, 4096u, bus[d].frame};
                        break;
                    }
                }
            }
            request.device_grants = device_grants;
            request.device_grant_count = device_grant_count;
        }
        /* A service that reads the boot image itself gets the same read-only
         * mapping a spawner does, without the spawn authority: the archive
         * is the volume the initrd service serves (specs/vfs.md). */
        if (entry.initrd) {
            request.binaries = initrd_.blob();
            request.binaries_bytes = static_cast<uint32_t>(initrd_.blob_size());
        }
        request.memory_frame = memory_frame;
        request.memory_bytes = memory_frame != 0 ? (1u << memory_bits) : 0;
        request.fault_endpoint = fault_endpoint_;
        /* Badges count from one so that zero keeps meaning "nobody in
         * particular" -- which is what director itself looks like. */
        request.badge = i + 1;

        spawn::Process process{};
        if (!spawner_.spawn(request, account, process)) {
            boot.problem = spawner_.problem();
            boot.detail = spawner_.detail();
            boot.error = spawner_.error();
            return;
        }
        started[boot.started].name = entry.name.data;
        started[boot.started].name_length = entry.name.length;
        started[boot.started].supervision = process.supervision;
        started[boot.started].tcb = process.tcb;
        started[boot.started].badge = request.badge;
        started[boot.started].entry = process.entry;
        if (supervisor != nullptr) {
            supervisor->record(boot.started, request.badge, process.tcb, process.supervision,
                               entry.name.data, entry.name.length);
        }
        ++boot.started;
    }
}

}  // namespace aegir::director
