/*
 * The virtio status handshake -- implementation. See include/aegir/virtio/handshake.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/virtio/handshake.h>

namespace aegir::virtio {

bool handshake(Registers const &registers, uint32_t *features_out) noexcept
{
    /* Reset first. A device someone else has been using -- or this one, last boot -- starts
     * from zero, and the spec makes the reset step one for exactly that reason. */
    registers.write(kStatus, 0);
    for (unsigned spin = 0; spin < 1000000 && registers.read(kStatus) != 0; ++spin) {
    }

    registers.write(kStatus, kStatusAcknowledge);
    registers.write(kStatus, kStatusAcknowledge | kStatusDriver);

    /* Features. A 64-bit value in a 32-bit register, low half first: the selector exists
     * only on the modern interface, and a legacy transport answers the low half and ignores
     * a selector write (virtio 1.x, 4.2.2 -- and QEMU hands out the legacy interface unless
     * asked for modern, which is why the version is read and branched on rather than
     * assumed). */
    bool const modern = registers.read(kVersion) == 2;
    uint32_t const features_low = registers.read(kDeviceFeatures);
    uint32_t features_high = 0;
    if (modern) {
        registers.write(kDeviceFeaturesSel, 1);
        features_high = registers.read(kDeviceFeatures);
        registers.write(kDeviceFeaturesSel, 0);
    }
    static_cast<void>(features_high);

    /* With one exception: VIRTIO_F_VERSION_1 is bit 32, and it is not a feature so much as the
     * handshake saying which interface we understood. It is written whether or not the version
     * register claims the modern interface, because this device's version register says 1
     * while it answers the modern register layout -- and a device that offers that layout will
     * not use a queue until the driver confirms it. Gating this on the version register is
     * exactly the assumption that left a queue set up, notified, and untouched: the status
     * byte's sentinel came back unchanged. */
    registers.write(kDriverFeatures, 0);
    registers.write(kDriverFeaturesSel, 1);
    registers.write(kDriverFeatures, 1); /* VIRTIO_F_VERSION_1 */
    registers.write(kDriverFeaturesSel, 0);

    registers.write(kStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOk);

    /* The device clears FEATURES_OK when it cannot live with what we asked for, so this
     * read is the check rather than a formality (virtio 1.x, 2.1.1 step 6). */
    if ((registers.read(kStatus) & kStatusFeaturesOk) == 0) {
        return false;
    }

    if (features_out != nullptr) {
        *features_out = features_low;
    }
    return true;
}

}  // namespace aegir::virtio
