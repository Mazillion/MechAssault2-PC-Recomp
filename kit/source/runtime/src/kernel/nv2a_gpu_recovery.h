#ifndef NV2A_GPU_RECOVERY_H
#define NV2A_GPU_RECOVERY_H
#include <stdint.h>

/* Device creation alone is not recovery: the replacement must also present
 * healthy frames. Keep one bounded retry budget through immediate relapses. */
enum Nv2aGpuRecoveryPhase { NV2A_GPU_READY, NV2A_GPU_LOST,
    NV2A_GPU_RESTORING, NV2A_GPU_VERIFYING, NV2A_GPU_STOPPED };
typedef struct {
    enum Nv2aGpuRecoveryPhase phase;
    unsigned attempts, healthy_presents;
    uint64_t started_ms, next_attempt_ms, verification_progress_ms;
} Nv2aGpuRecovery;
/* T2-3 B2.2 (R3 E1): 8 tries over 10 s gave up on a driver update or TDR that takes longer. */
#define NV2A_GPU_RECOVERY_ATTEMPTS 30u
#define NV2A_GPU_RECOVERY_TIMEOUT_MS 60000u
#define NV2A_GPU_RECOVERY_DELAY_MS 1000u
#define NV2A_GPU_RECOVERY_HEALTHY_FRAMES 60u

static int nv2a_gpu_device_loss_status(uint32_t status, uint32_t removal_reason)
{
    /* DXGI removed/hung/reset/driver-internal-error. An ordinary bad argument
     * or allocation failure on a healthy device must not trigger recreation. */
    return (int32_t)removal_reason < 0 || status == 0x887A0005u ||
        status == 0x887A0006u || status == 0x887A0007u || status == 0x887A0020u;
}
static void nv2a_gpu_recovery_lost(Nv2aGpuRecovery *r, uint64_t now)
{
    if (r->phase == NV2A_GPU_STOPPED || r->phase == NV2A_GPU_LOST) return;
    if (r->phase == NV2A_GPU_READY) {
        r->started_ms = now;
        r->attempts = 0;
    }
    r->phase = NV2A_GPU_LOST;
    r->healthy_presents = 0;
    r->next_attempt_ms = now;
}
/* 1: try once at this boundary; 0: keep draining commands; -1: stop retrying. */
static int nv2a_gpu_recovery_poll(Nv2aGpuRecovery *r, uint64_t now)
{
    if (r->phase == NV2A_GPU_STOPPED) return -1;
    if (r->phase == NV2A_GPU_VERIFYING) {
        if (now >= r->verification_progress_ms &&
            now - r->verification_progress_ms >= NV2A_GPU_RECOVERY_TIMEOUT_MS) {
            r->phase = NV2A_GPU_STOPPED;
            return -1;
        }
        return 0;
    }
    if (r->phase != NV2A_GPU_LOST) return 0;
    if (r->attempts >= NV2A_GPU_RECOVERY_ATTEMPTS ||
        (now >= r->started_ms && now - r->started_ms >= NV2A_GPU_RECOVERY_TIMEOUT_MS)) {
        r->phase = NV2A_GPU_STOPPED;
        return -1;
    }
    if (now < r->next_attempt_ms) return 0;
    ++r->attempts;
    r->phase = NV2A_GPU_RESTORING;
    return 1;
}
static void nv2a_gpu_recovery_retry(Nv2aGpuRecovery *r, uint64_t now)
{
    if (r->phase == NV2A_GPU_STOPPED) return;
    r->phase = NV2A_GPU_LOST;
    r->next_attempt_ms = now + NV2A_GPU_RECOVERY_DELAY_MS;
}
static void nv2a_gpu_recovery_created(Nv2aGpuRecovery *r, uint64_t now)
{
    r->phase = NV2A_GPU_VERIFYING;
    r->healthy_presents = 0;
    r->verification_progress_ms = now;
}
static int nv2a_gpu_recovery_presented(Nv2aGpuRecovery *r, uint64_t now)
{
    if (r->phase != NV2A_GPU_VERIFYING) return 0;
    r->verification_progress_ms = now;
    if (++r->healthy_presents < NV2A_GPU_RECOVERY_HEALTHY_FRAMES) return 0;
    r->phase = NV2A_GPU_READY;
    return 1;
}
static void nv2a_gpu_recovery_suspended(Nv2aGpuRecovery *r, uint64_t now)
{
    if (r->phase != NV2A_GPU_VERIFYING) return;
    /* Intentional display suspension is not failed renderer progress. Keep
     * the retry count, but do not consume its time budget while minimized. */
    if (now > r->verification_progress_ms)
        r->started_ms += now - r->verification_progress_ms;
    r->verification_progress_ms = now;
}
static int nv2a_gpu_recovery_discard(const Nv2aGpuRecovery *r)
{
    return r->phase == NV2A_GPU_LOST || r->phase == NV2A_GPU_RESTORING ||
        r->phase == NV2A_GPU_STOPPED;
}
#endif
