// Diagnostic-only executable: run the ORIGINAL engine and feed fixed input
// edges through its host APIs. Never writes engine/UI state to fake a result.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "EngineVisionBridge.h"
#include "metalrenderer.h"
#include "gamecontroller.h"
#include "directsound.h"
#include "pointer.h"
#include "host.h"
#include <unistd.h>
#include <stdatomic.h>
#include <limits.h>
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <os/lock.h>

static NSDictionary *live_guest_pose(void);

// Optional file mailbox, read only on this executable's main thread. Each
// command replaces the whole pad; rereading it never renews its short lease.
static NSString *probe_input_file;
static atomic_bool probe_live_control;
static uint64_t live_sequence, live_injection_sequence = 1;
static double live_deadline;
static bool live_active;
static NSDictionary *live_command, *live_snapshot;
static NSString *live_error;
static double probe_monotonic(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static double probe_wall_ms(void) { return [NSDate date].timeIntervalSince1970 * 1000; }
static void live_neutral(void) {
    if (!live_active) return;
    HostGCSnapshot pad = {.connected=true, .sequence=++live_injection_sequence};
    hostgc_inject_test_snapshot(&pad); live_active=false;
    host_pointer_set(0,0,0,HOST_POINTER_CANCEL);
}
static bool live_number(id value, double low, double high, double *out) {
    if (![value isKindOfClass:[NSNumber class]] || CFGetTypeID((__bridge CFTypeRef)value)==CFBooleanGetTypeID()) return false;
    double n=[value doubleValue];
    if (!isfinite(n) || n<low || n>high) return false;
    *out=n; return true;
}
static bool live_keys(id value, NSArray *allowed) {
    if (![value isKindOfClass:[NSDictionary class]]) return false;
    for (id key in value) if (![allowed containsObject:key]) return false;
    return true;
}
static NSDictionary *live_read(void) {
    // A FIFO, oversized file, or partial write must not block the deadman.
    int fd=open(probe_input_file.fileSystemRepresentation, O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return nil;
    struct stat st; unsigned char bytes[16385]; size_t used=0; bool good=false;
    if (fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<=16384) {
        while (used<sizeof bytes) {
            ssize_t n=read(fd,bytes+used,sizeof bytes-used);
            if(n<0 && errno==EINTR) continue;
            if(n<0) break;
            if(n==0) { good=used>0 && used<=16384; break; }
            used+=(size_t)n;
        }
    }
    close(fd);
    if(!good) return nil;
    id json=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bytes length:used] options:0 error:NULL];
    return [json isKindOfClass:[NSDictionary class]] ? json : nil;
}
static bool live_parse(NSDictionary *json, HostGCSnapshot *pad, uint64_t *sequence, double *issued, double *ttl) {
    if(!live_keys(json,@[@"sequence",@"issued_at_ms",@"timeout_ms",@"buttons",@"sticks",@"triggers",@"dpad",@"snapshot",@"tap"])) return false;
    double seq;
    if(!live_number(json[@"sequence"],1,9007199254740991.0,&seq) || floor(seq)!=seq ||
       !live_number(json[@"issued_at_ms"],1,9007199254740991.0,issued)) return false;
    *sequence=(uint64_t)seq; *ttl=500;
    if(json[@"timeout_ms"] && !live_number(json[@"timeout_ms"],20,2000,ttl)) return false;
    if(json[@"snapshot"] && CFGetTypeID((__bridge CFTypeRef)json[@"snapshot"])!=CFBooleanGetTypeID()) return false;
    if(json[@"tap"]) {
        id tap=json[@"tap"]; double value;
        if(![tap isKindOfClass:[NSArray class]] || [tap count]!=2 ||
           !live_number(tap[0],0,1,&value) || !live_number(tap[1],0,1,&value)) return false;
    }
    memset(pad,0,sizeof *pad); pad->connected=true;
    NSArray *names=@[@"A",@"B",@"X",@"Y",@"LSHOULDER",@"RSHOULDER",@"LTRIGGER",@"RTRIGGER",@"LTHUMB",@"RTHUMB",@"MENU",@"OPTIONS",@"HOME"];
    id buttons=json[@"buttons"] ?: @[];
    if(![buttons isKindOfClass:[NSArray class]]) return false;
    for(id name in buttons) {
        NSUInteger index=[names indexOfObject:name];
        if(index==NSNotFound) return false;
        pad->buttons[index]=true;
    }
    NSArray *axisNames=@[@"lx",@"ly",@"rx",@"ry",@"lt",@"rt"];
    float *axes[]={&pad->lx,&pad->ly,&pad->rx,&pad->ry,&pad->lt,&pad->rt};
    id sticks=json[@"sticks"] ?: @{}, triggers=json[@"triggers"] ?: @{};
    if(!live_keys(sticks,@[@"lx",@"ly",@"rx",@"ry"]) || !live_keys(triggers,@[@"lt",@"rt"])) return false;
    for(unsigned i=0;i<6;i++) {
        id n=(i<4 ? sticks : triggers)[axisNames[i]]; double value;
        if(n) { if(!live_number(n,i<4 ? -1 : 0,1,&value)) return false; *axes[i]=(float)value; }
    }
    if(pad->buttons[HOSTGC_BTN_LTRIGGER]) pad->lt=1;
    if(pad->buttons[HOSTGC_BTN_RTRIGGER]) pad->rt=1;
    pad->buttons[HOSTGC_BTN_LTRIGGER]=pad->lt>0.5f;
    pad->buttons[HOSTGC_BTN_RTRIGGER]=pad->rt>0.5f;
    id dpad=json[@"dpad"] ?: @[];
    NSArray *directions=@[@"up",@"down",@"left",@"right"];
    bool *directionsOut[]={&pad->dpad_up,&pad->dpad_down,&pad->dpad_left,&pad->dpad_right};
    if(![dpad isKindOfClass:[NSArray class]]) return false;
    for(id name in dpad) {
        NSUInteger index=[directions indexOfObject:name];
        if(index==NSNotFound) return false;
        *directionsOut[index]=true; pad->buttons[HOSTGC_BTN_DPAD_UP+index]=true;
    }
    return !(pad->dpad_up && pad->dpad_down) && !(pad->dpad_left && pad->dpad_right);
}
static bool live_panorama_valid(EngineVisionPanoramaInfo p, size_t capacity) {
    return p.sequence && p.width>0 && p.height>0 && p.width<=8192 && p.height<=8192 &&
        p.byte_count==(size_t)p.width*p.height*4*4 && p.byte_count<=256u*1024u*1024u && p.byte_count<=capacity;
}
static void live_capture_hud(NSDictionary *command, NSString *dir, NSString *base, uint64_t original_sequence) {
    EngineVisionPanoramaInfo p={0}; NSMutableData *panorama=nil;
    for(int attempt=0;attempt<3;attempt++) {
        if(!enginevision_panorama_info(&p)) {
            fprintf(stderr,"[live-input] HUD unavailable for sequence=%llu\n",[command[@"sequence"] unsignedLongLongValue]); return;
        }
        if(!live_panorama_valid(p,SIZE_MAX)) break;
        panorama=[NSMutableData dataWithLength:p.byte_count];
        if(enginevision_copy_panorama(panorama.mutableBytes,panorama.length,&p)) {
            // Panorama and ordinary frame are copied separately. This copy's
            // sequence/dimensions describe all four planes, including HUD.
            if(!live_panorama_valid(p,panorama.length)) break;
            if(command[@"probe_snapshot_at"] && (p.sequence!=original_sequence || p.flat_sequence!=original_sequence)){
                fprintf(stderr,"[probe-snapshot] rejected mismatched panorama=%llu flat=%llu requested=%llu\n",p.sequence,p.flat_sequence,original_sequence);return;
            }
            size_t plane_bytes=p.byte_count/4;
            NSData *hud=[panorama subdataWithRange:NSMakeRange(3*plane_bytes,plane_bytes)];
            NSDictionary *metadata=@{@"input_sequence":command[@"sequence"],@"frame_sequence":@(p.sequence),@"panorama_sequence":@(p.sequence),@"original_frame_sequence":@(original_sequence),@"width":@(p.width),@"height":@(p.height),@"byte_count":@(plane_bytes),@"panorama_byte_count":@(p.byte_count),@"plane_index":@3,@"plane_count":@4,@"source_epoch":@(p.source_epoch),@"flat_sequence":@(p.flat_sequence),@"status":@(p.status),@"failure_reason":@(p.failure_reason),@"pixel_format":@"BGRA8",@"captured_at_unix_ms":@(probe_wall_ms())};
            NSError *error=nil;
            NSData *json=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:&error];
            bool ok=[hud writeToFile:[dir stringByAppendingPathComponent:[base stringByAppendingString:@".hud.bgra"]] options:NSDataWritingAtomic error:&error] &&
                [json writeToFile:[dir stringByAppendingPathComponent:[base stringByAppendingString:@".hud.json"]] options:NSDataWritingAtomic error:&error];
            fprintf(stderr,"[live-input] HUD sequence=%llu panorama_frame=%llu %s\n",[command[@"sequence"] unsignedLongLongValue],p.sequence,ok ? base.UTF8String : error.localizedDescription.UTF8String);
            if(getenv("HALO_PROBE_CAPTURE_PANORAMA") || command[@"probe_snapshot_at"]){
                for(unsigned plane=0;plane<3;plane++){
                    NSString *name=[base stringByAppendingFormat:@".panorama-%u",plane];
                    NSData *layer=[panorama subdataWithRange:NSMakeRange(plane*plane_bytes,plane_bytes)];
                    NSDictionary *info=@{@"width":@(p.width),@"height":@(p.height),@"frame_sequence":@(p.sequence),@"source_epoch":@(p.source_epoch),@"flat_sequence":@(p.flat_sequence),@"status":@(p.status),@"failure_reason":@(p.failure_reason),@"plane_index":@(plane),@"projection_x":@(p.projection_x[plane]),@"projection_y":@(p.projection_y[plane]),@"viewport_uv":@[@(p.viewport_u_min[plane]),@(p.viewport_v_min[plane]),@(p.viewport_u_max[plane]),@(p.viewport_v_max[plane])]};
                    NSData *sidecar=[NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingPrettyPrinted error:&error];
                    bool saved=[layer writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".bgra"]] options:NSDataWritingAtomic error:&error] && [sidecar writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".json"]] options:NSDataWritingAtomic error:&error];
                    if(!saved)fprintf(stderr,"[live-input] panorama capture failed: %s\n",error.localizedDescription.UTF8String);
                }
            }
            return;
        }
    }
    fprintf(stderr,"[live-input] HUD copy failed or invalid metadata for sequence=%llu\n",[command[@"sequence"] unsignedLongLongValue]);
}
/* The byte-copy buffers can be old menu pixels while the zero-copy path is
 * rendering a world. Interactive diagnostics must capture the same leased
 * textures the headset receives, with their own publication and image epochs. */
static bool live_capture_gpu(NSDictionary *command, NSString *directory) {
    EngineVisionPanoramaGPUSnapshot lent={0};
    if(!enginevision_panorama_gpu_latest(&lent))return false;
    bool saved=false;
    @try {
        id<MTLDevice> device=(__bridge id<MTLDevice>)mr_shared_device();
        id<MTLCommandQueue> queue=device ? [device newCommandQueue] : nil;
        int w=lent.info.width,h=lent.info.height;
        if(!queue || w<=0 || h<=0 || w>8192 || h>8192)return false;
        if(command[@"probe_snapshot_at"] && lent.info.sequence!=[command[@"probe_snapshot_at"] unsignedLongLongValue])return false;
        size_t bytes=(size_t)w*(size_t)h*4;
        id<MTLBuffer> staging=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!staging)return false;
        NSString *base=[NSString stringWithFormat:@"live-input-%@-frame-%llu",command[@"sequence"],lent.info.sequence];
        NSDictionary *pose=live_guest_pose();
        NSError *error=nil;
        if(![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error])return false;
        saved=true;
        const int layers[]={HALO_PANORAMA_CENTRE_LEFT,HALO_PANORAMA_HUD_LAYER};
        for(unsigned i=0;i<sizeof layers/sizeof *layers;i++) {
            int k=layers[i];id<MTLTexture> texture=(__bridge id<MTLTexture>)lent.textures[k];
            if(!texture || texture.width!=(NSUInteger)w || texture.height!=(NSUInteger)h){saved=false;break;}
            id<MTLCommandBuffer> cb=[queue commandBuffer];
            id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
            if(!cb || !blit){saved=false;break;}
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(w,h,1) toBuffer:staging destinationOffset:0
                destinationBytesPerRow:(NSUInteger)w*4 destinationBytesPerImage:bytes];
            [blit endEncoding];[cb commit];[cb waitUntilCompleted];
            if(cb.status!=MTLCommandBufferStatusCompleted){saved=false;break;}
            NSString *path=[directory stringByAppendingPathComponent:i ? [base stringByAppendingString:@".hud"] : base];
            NSMutableDictionary *metadata=[@{@"input_sequence":command[@"sequence"],@"command":command,
                @"frame_sequence":@(lent.info.sequence),@"width":@(w),@"height":@(h),@"byte_count":@(bytes),
                @"pixel_format":@"BGRA8",@"capture_source":@"leased_gpu",@"plane_index":@(k),
                @"source_epoch":@(lent.info.source_epoch),@"scene_epoch":@(lent.info.scene_epoch),
                @"image_epoch":@(lent.info.layer_epoch[k]),@"flat_sequence":@(lent.info.flat_sequence),
                @"status":@(lent.info.status),@"stereo":@(lent.info.stereo),
                @"projection_x":@(lent.info.projection_x[k]),@"projection_y":@(lent.info.projection_y[k]),
                @"viewport_uv":@[@(lent.info.viewport_u_min[k]),@(lent.info.viewport_v_min[k]),@(lent.info.viewport_u_max[k]),@(lent.info.viewport_v_max[k])],
                @"captured_at_unix_ms":@(probe_wall_ms())} mutableCopy];
            if(pose)metadata[@"guest_pose"]=pose;
            NSData *json=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:&error];
            NSData *pixels=[NSData dataWithBytes:staging.contents length:bytes];
            if(!json || ![pixels writeToFile:[path stringByAppendingString:@".bgra"] options:NSDataWritingAtomic error:&error] ||
                ![json writeToFile:[path stringByAppendingString:@".json"] options:NSDataWritingAtomic error:&error]){saved=false;break;}
        }
        fprintf(stderr,"[live-input] GPU snapshot sequence=%llu publication=%llu %s\n",
            [command[@"sequence"] unsignedLongLongValue],lent.info.sequence,saved ? base.UTF8String : "failed");
    } @finally {
        /* Includes allocation, validation, readback and file-write failures. */
        enginevision_panorama_gpu_release(lent.slot);
    }
    return saved;
}
static bool live_capture(NSDictionary *command) {
    const char *directory=getenv("HALO_FRAME_CAPTURE");
    if(!directory || !*directory) { fprintf(stderr,"[live-input] snapshot requires HALO_FRAME_CAPTURE\n"); return true; }
    if(enginevision_panorama_gpu_enabled()) {
        EngineVisionPanoramaInfo info={0};enginevision_panorama_info(&info);
        if(info.status!=HALO_PANORAMA_FLAT)
            return live_capture_gpu(command,[NSString stringWithUTF8String:directory]);
    }
    EngineVisionFrameInfo f={0}; NSMutableData *pixels=nil;
    for(int attempt=0;attempt<3;attempt++) {
        if(!enginevision_frame_info(&f) || !f.sequence) return false;
        if(f.width<=0 || f.height<=0 || f.width>8192 || f.height>8192 || f.byte_count!=(size_t)f.width*f.height*4) return true;
        pixels=[NSMutableData dataWithLength:f.byte_count];
        if(enginevision_copy_latest_frame(pixels.mutableBytes,pixels.length,&f)) {
            // The frame may change between info and copy. Use copy's metadata.
            if(f.width<=0 || f.height<=0 || f.width>8192 || f.height>8192 || f.byte_count!=(size_t)f.width*f.height*4 || f.byte_count>pixels.length) return true;
            pixels.length=f.byte_count; break;
        }
        pixels=nil;
    }
    if(!pixels) return false;
    if(command[@"probe_snapshot_at"] && f.sequence!=[command[@"probe_snapshot_at"] unsignedLongLongValue]){
        fprintf(stderr,"[probe-snapshot] rejected mismatched frame=%llu requested=%llu\n",f.sequence,[command[@"probe_snapshot_at"] unsignedLongLongValue]);return false;
    }
    NSString *dir=[NSString stringWithUTF8String:directory]; NSError *error=nil;
    NSString *base=command[@"probe_snapshot_at"] ? [NSString stringWithFormat:@"snapshot-at-%llu",f.sequence] : [NSString stringWithFormat:@"live-input-%@-frame-%llu",command[@"sequence"],f.sequence];
    char status[512]; enginevision_copy_status(status,sizeof status);
    NSMutableDictionary *metadata=[@{@"input_sequence":command[@"sequence"],@"frame_sequence":@(f.sequence),@"width":@(f.width),@"height":@(f.height),@"byte_count":@(f.byte_count),@"pixel_format":@"BGRA8",@"captured_at_unix_ms":@(probe_wall_ms()),@"status":[NSString stringWithUTF8String:status] ?: @"",@"command":command} mutableCopy];
    // Read-only test telemetry, copied atomically from the engine thread. Its
    // own sequence is explicit: a newer image can arrive before its pose copy.
    NSDictionary *pose=live_guest_pose();
    if(pose)metadata[@"guest_pose"]=pose;
    NSData *json=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:&error];
    bool ok=[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&error] &&
        [pixels writeToFile:[dir stringByAppendingPathComponent:[base stringByAppendingString:@".bgra"]] options:NSDataWritingAtomic error:&error] &&
        [json writeToFile:[dir stringByAppendingPathComponent:[base stringByAppendingString:@".json"]] options:NSDataWritingAtomic error:&error];
    fprintf(stderr,"[live-input] snapshot sequence=%llu frame=%llu %s\n",[command[@"sequence"] unsignedLongLongValue],f.sequence,ok ? base.UTF8String : error.localizedDescription.UTF8String);
    if(ok) live_capture_hud(command,dir,base,f.sequence);
    return true;
}
/* Test executable only. Engine-thread callback runs after publication and
 * before another Present can advance it. Never injects input or guest writes. */
static struct { bool initialized,finished;uint64_t target; } probe_snapshot;
static void probe_snapshot_present(uint64_t sequence) {
    if(!probe_snapshot.initialized){
        probe_snapshot.initialized=true;const char *value=getenv("HALO_PROBE_SNAPSHOT_AT");
        if(value&&*value){
            bool digits=true;for(const char *p=value;*p;p++)if(*p<'0'||*p>'9')digits=false;
            char *end=NULL;errno=0;unsigned long long target=strtoull(value,&end,10);
            if(digits&&!errno&&end!=value&&!*end&&target>0&&target<=INT_MAX)probe_snapshot.target=target;
            else fprintf(stderr,"[probe-snapshot] invalid HALO_PROBE_SNAPSHOT_AT; disabled\n");
        }
    }
    if(!probe_snapshot.target||probe_snapshot.finished||sequence<probe_snapshot.target)return;
    probe_snapshot.finished=true;
    if(sequence!=probe_snapshot.target){fprintf(stderr,"[probe-snapshot] missed requested=%llu published=%llu; no later-frame substitution\n",probe_snapshot.target,sequence);return;}
    @autoreleasepool {
        NSDictionary *command=@{@"sequence":@(sequence),@"probe_snapshot_at":@(sequence),@"snapshot":@YES,@"scope":@"Read-only post-publication test snapshot; no input injection"};
        fprintf(stderr,"[probe-snapshot] requested=%llu published=%llu\n",probe_snapshot.target,sequence);
        if(!live_capture(command))fprintf(stderr,"[probe-snapshot] exact frame copy failed; single shot consumed\n");
    }
}
static void live_poll(double now, double wall_ms) {
    if(live_active && now>=live_deadline) live_neutral();
    NSDictionary *json=live_read(); HostGCSnapshot pad; uint64_t sequence=0; double issued=0,ttl=0;
    NSString *error=nil;
    if(!live_parse(json,&pad,&sequence,&issued,&ttl)) error=@"missing or malformed command; neutral";
    else if(sequence<live_sequence || (sequence==live_sequence && ![json isEqual:live_command])) error=@"sequence must increase when command changes; neutral";
    else if(sequence>live_sequence) {
        // Consume stale commands too: they cannot later rearm due to a clock change.
        live_sequence=sequence; live_command=json;
        double age=wall_ms-issued;
        if(age < -250 || age>=ttl) error=@"expired or future command; neutral";
        else {
            pad.sequence=++live_injection_sequence; hostgc_inject_test_snapshot(&pad);
            // Exercise the same completed-pinch queue as the headset. No
            // cursor/widget memory writes; the original engine handles clicks.
            if(json[@"tap"]) host_pointer_set([json[@"tap"][0] floatValue],[json[@"tap"][1] floatValue],1,HOST_POINTER_TAP);
            live_active=true; live_deadline=now+fmin(ttl,ttl-age)/1000.0;
            if([json[@"snapshot"] boolValue]) live_snapshot=json;
            fprintf(stderr,"[live-input] accepted sequence=%llu timeout_ms=%.0f\n",sequence,ttl);
        }
    }
    if(error) {
        live_neutral();
        if(![error isEqual:live_error]) fprintf(stderr,"[live-input] %s\n",error.UTF8String);
    }
    live_error=error;
    if(live_snapshot && live_capture(live_snapshot)) live_snapshot=nil;
}
extern int host_frame_limit;
extern uint8_t host_keyboard_state[256];
extern uint64_t host_dinput_gamepad_reads(void);
static int probe_mode;
extern uint64_t host_dinput_keyboard_events(void);
extern uint8_t *engine_flat_base;
static uint32_t guest_u32(uint32_t address) { uint32_t v;memcpy(&v,engine_flat_base+address,4);return v; }
typedef struct {
    uint64_t sequence;
    uint32_t tick, player, unit;
    bool shell, hasUnit, menuActive;
    uint32_t menuRoot;
    uint16_t pauseDepth, gameMode;
    float observer[3], forward[3], origin[3], center[3];
    bool hasControls, hasWeapon, hasPlayerGlobals, inputDisabled, padConnected;
    uint8_t inputBlock[2], mouseButtons[8], hardwareInput[40];
    HostGCSnapshot pad;
    uint32_t observed, queried, held, controlled, unitCommand, weapon, definition;
    uint32_t itemFlags, weaponFlags, triggerFlags;
    int16_t weaponSlot, desiredSlot, triggerTicks, magazineState, magazineTicks,
            magazineTotalTicks, reservedRounds, loadedRounds;
    int8_t triggerElapsed;
    uint8_t triggerState;
    float weaponAnalog, weaponAge, triggerRate;
} ProbeGuestPose;
static os_unfair_lock guest_pose_lock=OS_UNFAIR_LOCK_INIT;
static ProbeGuestPose guest_pose;
static bool guest_pose_range(uint64_t address,size_t bytes) {
    return engine_flat_base && address>=0x10000u && address<=UINT32_MAX &&
           bytes<=UINT64_C(0x100000000)-address;
}
static uint16_t guest_u16(uint32_t address) { uint16_t v;memcpy(&v,engine_flat_base+address,2);return v; }
static float guest_float(uint32_t address) { float v;memcpy(&v,engine_flat_base+address,4);return v; }
/* Original object table: capacity +20, entry size +22, data +34; entry salt +0,
 * body byte count +6 and pointer +8. Reject stale handles before body reads. */
static uint32_t guest_pose_object(uint32_t table,uint32_t datum,size_t bytes,int kind) {
    if(datum==UINT32_MAX || !guest_pose_range(table,0x38))return 0;
    unsigned index=datum&0xffffu,capacity=guest_u16(table+0x20);
    if(!capacity || capacity>4096 || index>=capacity || guest_u16(table+0x22)!=12)return 0;
    uint32_t entries=guest_u32(table+0x34);
    if(!guest_pose_range(entries,(size_t)capacity*12))return 0;
    uint64_t entry=(uint64_t)entries+(uint64_t)index*12;
    if(!guest_pose_range(entry,12) || !guest_u16((uint32_t)entry) ||
       guest_u16((uint32_t)entry)!=(datum>>16) || guest_u16((uint32_t)entry+6)<bytes)return 0;
    uint32_t body=guest_u32((uint32_t)entry+8);
    if(!guest_pose_range(body,bytes) || bytes<0xb6)return 0;
    unsigned actual=guest_u16(body+0xb4);
    if(kind<0 ? actual>1 : actual!=(unsigned)kind)return 0;
    return body;
}
extern void host_dinput_mouse_buttons_state(uint8_t out[8]);
/* Fixed-size observations copied on the engine's post-publication callback.
 * Fresh host polls are labelled separately from the engine's persistent latches;
 * the transient 00472760 stack packet is intentionally not guessed here. */
static void capture_guest_weapon(ProbeGuestPose *p,uint32_t unitBody,uint32_t objects) {
    p->weaponSlot=(int16_t)guest_u16(unitBody+0x2f2);
    p->desiredSlot=(int16_t)guest_u16(unitBody+0x2f4);
    p->unitCommand=guest_u32(unitBody+0x208);
    if(p->weaponSlot<0 || p->weaponSlot>=4)return;
    p->weapon=guest_u32(unitBody+0x2f8+(unsigned)p->weaponSlot*4);
    uint32_t w=guest_pose_object(objects,p->weapon,0x2bc,2);
    if(!w)return;
    p->hasWeapon=true;p->definition=guest_u32(w);
    p->itemFlags=guest_u32(w+0x1f4);p->weaponFlags=guest_u32(w+0x22c);
    p->weaponAnalog=guest_float(w+0x234);p->weaponAge=guest_float(w+0x240);
    p->triggerElapsed=(int8_t)engine_flat_base[w+0x260];p->triggerState=engine_flat_base[w+0x261];
    p->triggerTicks=(int16_t)guest_u16(w+0x262);p->triggerFlags=guest_u32(w+0x264);
    p->triggerRate=guest_float(w+0x270);
    p->magazineState=(int16_t)guest_u16(w+0x2b0);p->magazineTicks=(int16_t)guest_u16(w+0x2b2);
    p->magazineTotalTicks=(int16_t)guest_u16(w+0x2b4);
    p->reservedRounds=(int16_t)guest_u16(w+0x2b6);p->loadedRounds=(int16_t)guest_u16(w+0x2b8);
}
static void capture_guest_pose(uint64_t sequence) {
    /* Timing/single-frame diagnostics also need real pose metadata before any
     * live mailbox takes ownership. This remains a read-only engine-thread copy. */
    static int observe_without_live=-1;
    if(observe_without_live<0) {
        const char *timing=getenv("HALO_PROBE_FRAME_TIMING");
        observe_without_live=(timing && !strcmp(timing,"1")) || getenv("HALO_PROBE_SNAPSHOT_AT");
    }
    if((!atomic_load(&probe_live_control) && !observe_without_live) || !engine_flat_base || !sequence) return;
    ProbeGuestPose p={.sequence=sequence,.player=UINT32_MAX,.unit=UINT32_MAX,.weapon=UINT32_MAX,.weaponSlot=-1,.desiredSlot=-1};
    p.shell=engine_flat_base[0x00718FC9u]!=0;
    p.menuActive=enginevision_menu_active()!=0;
    p.menuRoot=guest_u32(0x00718F94u);
    p.pauseDepth=guest_u16(0x00718FA6u);
    p.gameMode=guest_u16(0x00719720u);
    memcpy(p.observer,engine_flat_base+0x006AC6D0u,sizeof p.observer);
    memcpy(p.forward,engine_flat_base+0x006AC6F0u,sizeof p.forward);
    uint32_t time=guest_u32(0x006F1D6Cu);
    if(guest_pose_range(time,16))p.tick=guest_u32(time+12);
    p.padConnected=hostgc_poll(&p.pad);
    host_dinput_mouse_buttons_state(p.mouseButtons);
    memcpy(p.hardwareInput,engine_flat_base+0x00712498u,sizeof p.hardwareInput);
    p.inputBlock[0]=engine_flat_base[0x006ac5b1];p.inputBlock[1]=engine_flat_base[0x006ac5b2];
    uint32_t controls=guest_u32(0x006b145c);
    if(guest_pose_range(controls,0x14)) {
        p.hasControls=true;p.observed=guest_u32(controls);p.queried=guest_u32(controls+4);
        p.held=guest_u32(controls+8);p.controlled=guest_u32(controls+0x10);
    }
    uint32_t players=guest_u32(0x0087A478u),array=guest_u32(0x0087A480u);
    if(guest_pose_range(players,0x12)) { p.hasPlayerGlobals=true;p.inputDisabled=engine_flat_base[players+0x11]!=0; }
    if(!p.shell && guest_pose_range(players,8) && guest_pose_range(array,0x38)) {
        p.player=guest_u32(players+4);
        uint32_t records=guest_u32(array+0x34);
        unsigned capacity=guest_u16(array+0x20);
        uint64_t record=(uint64_t)records+(p.player&0xffffu)*0x200u;
        if(p.player!=UINT32_MAX && capacity>0 && capacity<=16 &&
           guest_pose_range(records,(size_t)capacity*0x200) && (p.player&0xffffu)<capacity &&
           guest_u16(array+0x22)==0x200 && guest_pose_range(record,0x200) &&
           guest_u16((uint32_t)record)==(p.player>>16)) {
            p.unit=guest_u32((uint32_t)record+0x34);
            uint32_t objects=guest_u32(0x008603B0u);
            uint32_t body=guest_pose_object(objects,p.unit,0x308,-1);
            if(body) {
                memcpy(p.origin,engine_flat_base+body+0x5C,sizeof p.origin);
                memcpy(p.center,engine_flat_base+body+0xA0,sizeof p.center);
                p.hasUnit=true;capture_guest_weapon(&p,body,objects);
            }
        }
    }
    os_unfair_lock_lock(&guest_pose_lock);guest_pose=p;os_unfair_lock_unlock(&guest_pose_lock);
}
static NSArray *pose_vector(const float *v) {
    if(!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) return nil;
    return @[@(v[0]),@(v[1]),@(v[2])];
}
static NSDictionary *live_guest_pose(void) {
    ProbeGuestPose p;
    os_unfair_lock_lock(&guest_pose_lock);p=guest_pose;os_unfair_lock_unlock(&guest_pose_lock);
    if(!p.sequence) return nil;
    NSMutableDictionary *result=[@{@"frame_sequence":@(p.sequence),@"tick":@(p.tick),
        @"ui_shell":@(p.shell),@"player_datum":@(p.player),@"unit_datum":@(p.unit),
        @"menu_active":@(p.menuActive),@"menu_root":@(p.menuRoot),
        @"pause_depth":@(p.pauseDepth),@"game_mode":@(p.gameMode),
        @"scope":@"Diagnostic observation only; no guest writes"} mutableCopy];
    NSArray *observer=pose_vector(p.observer),*forward=pose_vector(p.forward);
    if(observer)result[@"observer"]=observer;
    if(forward)result[@"forward"]=forward;
    if(p.hasUnit) {
        NSArray *origin=pose_vector(p.origin),*center=pose_vector(p.center);
        if(origin)result[@"unit_origin"]=origin;
        if(center)result[@"unit_bounding_center"]=center;
    }
    NSMutableDictionary *input=[@{@"player_globals_valid":@(p.hasPlayerGlobals),@"actions_valid":@(p.hasControls),
        @"player_input_disabled":p.hasPlayerGlobals?@(p.inputDisabled):[NSNull null],
        @"camera_input_block":@[@(p.inputBlock[0]),@(p.inputBlock[1])],
        @"host_poll_connected":@(p.padConnected),@"host_poll_sequence":@(p.pad.sequence),
        @"host_poll_rt":isfinite(p.pad.rt)?@(p.pad.rt):[NSNull null],
        @"host_poll_rtrigger":@(p.pad.buttons[HOSTGC_BTN_RTRIGGER]),
        @"host_merged_mouse_left":@(p.mouseButtons[0]),
        @"host_poll_scope":@"Fresh observation; not the last original-engine DirectInput poll",
        @"hardware_input_00712498_base64":[[NSData dataWithBytes:p.hardwareInput length:40] base64EncodedStringWithOptions:0],
        @"transient_primary_packet":@"Not available at Present; requires 00472760 entry trace"} mutableCopy];
    if(p.hasControls) {
        input[@"actions_observed"]=@(p.observed);input[@"actions_queried"]=@(p.queried);
        input[@"actions_held"]=@(p.held);input[@"primary_trigger_observed"]=@((p.observed&0x10)!=0);
        input[@"controlled_unit_datum"]=@(p.controlled);
    }
    result[@"input"]=input;
    result[@"unit_valid"]=@(p.hasUnit);result[@"weapon_valid"]=@(p.hasWeapon);
    if(p.hasUnit)result[@"unit_weapon_selection"]=@{@"selected_slot":@(p.weaponSlot),
        @"desired_slot":@(p.desiredSlot),@"command_flags_0208":@(p.unitCommand),@"weapon_datum":@(p.weapon)};
    if(p.hasWeapon) {
        NSMutableDictionary *weapon=[@{@"datum":@(p.weapon),@"definition_datum":@(p.definition),
            @"item_flags_01f4":@(p.itemFlags),@"weapon_flags_022c":@(p.weaponFlags),
            @"trigger0":@{@"elapsed_ticks":@(p.triggerElapsed),@"state":@(p.triggerState),
                @"remaining_ticks":@(p.triggerTicks),@"flags":@(p.triggerFlags)},
            @"magazine0":@{@"state":@(p.magazineState),@"remaining_ticks":@(p.magazineTicks),
                @"total_ticks":@(p.magazineTotalTicks),@"reserved_rounds":@(p.reservedRounds),@"loaded_rounds":@(p.loadedRounds)}} mutableCopy];
        if(isfinite(p.weaponAnalog))weapon[@"analog_0234"]=@(p.weaponAnalog);
        if(isfinite(p.weaponAge))weapon[@"age_0240"]=@(p.weaponAge);
        if(isfinite(p.triggerRate))weapon[@"trigger_rate_0270"]=@(p.triggerRate);
        result[@"weapon"]=weapon;
    }
    return result;
}
/* Census of a published zero-copy slot, desktop only.
 *
 * The compositor refuses a frame unless every layer of the slot it leases
 * holds a picture, and on the headset that refusal is silent -- it simply
 * shows nothing. Reporting it here turns a class of black screen into a line
 * of text. It also measures what the rotating schedule needs: layers persist
 * between frames in the host's CPU buffers, but each frame publishes a
 * different pool slot, so any layer a frame does not redraw has to be carried
 * into the new slot or it comes back stale, and eventually black.
 *
 * HALO_PROBE_GPU_CENSUS=<n> reports every n published frames.
 * HALO_PROBE_GPU_CAPTURE_AT=<n[,n...]> saves the actual leased layers at the first
 * census observation at/after n, with their own publication/image epochs. Ordinary CPU frame
 * captures can still show the previous menu while zero-copy world frames run.
 */
static void probe_panorama_gpu_census(uint64_t sequence) {
    static int every=-1;
    static uint64_t last_census;
    static const char *next_capture;
    static bool capture_initialized;
    if(every<0){const char *v=getenv("HALO_PROBE_GPU_CENSUS");every=v?atoi(v):0;}
    if(every<=0||!enginevision_panorama_gpu_enabled())return;
    if(sequence>=last_census && sequence-last_census<(uint64_t)every)return;
    last_census=sequence;
    EngineVisionPanoramaGPUSnapshot lent;memset(&lent,0,sizeof lent);
    if(!enginevision_panorama_gpu_latest(&lent)){
        fprintf(stderr,"[probe-gpu] frame=%llu no complete slot to lease\n",(unsigned long long)sequence);return;}
    @autoreleasepool {
        id<MTLDevice> device=(__bridge id<MTLDevice>)mr_shared_device();
        static id<MTLCommandQueue> queue;
        if(device&&!queue)queue=[device newCommandQueue];
        int w=lent.info.width,h=lent.info.height;
        if(!device||!queue||w<=0||h<=0){enginevision_panorama_gpu_release(lent.slot);return;}
        /* The compositor's own acceptance test (Projections.isValid in
         * EnginePanoramaTexture.swift), applied here to the same info the
         * lease carries. Pixels alone said nothing about it: a slot can hold
         * a complete picture whose metadata the compositor will throw out, and
         * under the rotating schedule it did, every frame, because a skipped
         * layer published a projection of zero. Keep this in step with the
         * Swift check; it is the only desktop coverage that check has. */
        unsigned invalid=0;
        {
            unsigned required=lent.info.stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
            const float *px=lent.info.projection_x,*py=lent.info.projection_y;
            const float *u0=lent.info.viewport_u_min,*v0=lent.info.viewport_v_min;
            const float *u1=lent.info.viewport_u_max,*v1=lent.info.viewport_v_max;
            for(int k=0;k<HALO_PANORAMA_LAYERS;k++){
                if(!(required&(1u<<k))||k==HALO_PANORAMA_HUD_LAYER)continue;
                int ok=isfinite(px[k])&&px[k]>0&&px[k]<3&&isfinite(py[k])&&py[k]>0&&py[k]<3
                    &&isfinite(u0[k])&&isfinite(v0[k])&&isfinite(u1[k])&&isfinite(v1[k])
                    &&u0[k]>=0&&v0[k]>=0&&u1[k]<=1&&v1[k]<=1&&u1[k]>u0[k]&&v1[k]>v0[k]
                    &&px[k]/(u1[k]-u0[k])<=1.7321f;
                if(!ok)invalid|=1u<<k;
            }
        }
        size_t pixels=(size_t)w*(size_t)h,bytes=pixels*4;
        id<MTLBuffer> staging=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        const char *captureDir=getenv("HALO_FRAME_CAPTURE");
        if(!capture_initialized){next_capture=getenv("HALO_PROBE_GPU_CAPTURE_AT");capture_initialized=true;}
        char *captureEnd=NULL;
        uint64_t requested_capture=next_capture?strtoull(next_capture,&captureEnd,10):0;
        bool capture=next_capture && captureEnd!=next_capture && captureDir && *captureDir && requested_capture<=sequence;
        if(capture)next_capture=*captureEnd==','?captureEnd+1:NULL;
        unsigned empty=0,stale=0,moved=0,frozen=0;
        /* Content fingerprint per layer, so a layer that stops being redrawn
         * reads as stale rather than quietly persisting. */
        static uint64_t seen[HALO_PANORAMA_LAYERS];
        static uint64_t seen_at[HALO_PANORAMA_LAYERS];
        char detail[768];size_t used=0;detail[0]=0;
        for(int k=0;k<HALO_PANORAMA_LAYERS&&staging;k++){
            id<MTLTexture> layer=(__bridge id<MTLTexture>)lent.textures[k];
            if(!layer||(int)layer.width!=w||(int)layer.height!=h){empty|=1u<<k;continue;}
            id<MTLCommandBuffer> cb=[queue commandBuffer];
            id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
            [blit copyFromTexture:layer sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                       sourceSize:MTLSizeMake((NSUInteger)w,(NSUInteger)h,1) toBuffer:staging
                destinationOffset:0 destinationBytesPerRow:(NSUInteger)w*4
              destinationBytesPerImage:bytes];
            [blit endEncoding];[cb commit];[cb waitUntilCompleted];
            if(cb.status!=MTLCommandBufferStatusCompleted){
                empty|=1u<<k;
                fprintf(stderr,"[probe-gpu] frame=%llu layer=%d readback failed: %s\n",
                    (unsigned long long)sequence,k,cb.error.localizedDescription.UTF8String ?: "unknown Metal failure");
                continue;
            }
            if(capture){
                NSString *base=[[NSString stringWithUTF8String:captureDir] stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"gpu-at-%llu-layer-%02d",(unsigned long long)sequence,k]];
                NSDictionary *info=@{@"requested_minimum_sequence":@(requested_capture),
                    @"guest_pose":live_guest_pose() ?: @{},
                    @"layer_pose":@[@(lent.info.layer_pose[k][0]),@(lent.info.layer_pose[k][1]),@(lent.info.layer_pose[k][2]),
                        @(lent.info.layer_pose[k][3]),@(lent.info.layer_pose[k][4]),@(lent.info.layer_pose[k][5]),
                        @(lent.info.layer_pose[k][6]),@(lent.info.layer_pose[k][7]),@(lent.info.layer_pose[k][8])],
                    @"observation_sequence":@(sequence),@"publication_sequence":@(lent.info.sequence),
                    @"source_epoch":@(lent.info.source_epoch),@"scene_epoch":@(lent.info.scene_epoch),
                    @"image_epoch":@(lent.info.layer_epoch[k]),@"flat_sequence":@(lent.info.flat_sequence),
                    @"width":@(w),@"height":@(h),@"byte_count":@(bytes),@"layer":@(k),@"stereo":@(lent.info.stereo),
                    @"pixel_format":@"BGRA8",@"projection_x":@(lent.info.projection_x[k]),@"projection_y":@(lent.info.projection_y[k]),
                    @"viewport_uv":@[@(lent.info.viewport_u_min[k]),@(lent.info.viewport_v_min[k]),@(lent.info.viewport_u_max[k]),@(lent.info.viewport_v_max[k])],
                    @"scope":@"Exact leased GPU layer; diagnostic readback stalls and is not headset display evidence"};
                NSError *error=nil;
                NSData *json=[NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingPrettyPrinted error:&error];
                NSData *image=[NSData dataWithBytes:staging.contents length:bytes];
                bool saved=json && [image writeToFile:[base stringByAppendingString:@".bgra"] options:NSDataWritingAtomic error:&error]
                    && [json writeToFile:[base stringByAppendingString:@".json"] options:NSDataWritingAtomic error:&error];
                fprintf(stderr,"[probe-gpu-capture] frame=%llu publication=%llu layer=%d epoch=%llu %s\n",
                    (unsigned long long)sequence,(unsigned long long)lent.info.sequence,k,
                    (unsigned long long)lent.info.layer_epoch[k],saved?"saved":(error.localizedDescription.UTF8String ?: "failed"));
            }
            const uint32_t *px=staging.contents;
            size_t lit=0;uint64_t fingerprint=1469598103934665603ULL;
            for(size_t p=0;p<pixels;p+=13){                 /* sparse: a check, not a metric */
                uint32_t v=px[p]&0x00FFFFFFu;
                if(v)lit++;
                fingerprint=(fingerprint^v)*1099511628211ULL;
            }
            if(!lit)empty|=1u<<k;
            if(fingerprint!=seen[k]){moved|=1u<<k;seen[k]=fingerprint;seen_at[k]=sequence;}
            else if(lit&&seen_at[k]&&sequence-seen_at[k]>=(uint64_t)every*2)frozen|=1u<<k;
            if(used<sizeof detail-56)
                used+=(size_t)snprintf(detail+used,sizeof detail-used,"%s%d:%zu",used?" ":"",k,lit);
        }
        /* Unchanged content cannot prove a layer went unrefreshed: on the main
         * menu the forward bearings animate while the rear is a still
         * starfield, so a rear layer is rightly identical frame after frame.
         * Only emptiness is decisive, so that alone fails the frame; a frozen
         * layer is worth printing, because during play it is how a starved
         * rotating layer first shows, but it is a hint and not a verdict. */
        stale=moved?frozen:0;
        enginevision_panorama_gpu_release(lent.slot);
        /* Only the layers the engine itself requires make a sphere. The HUD
         * is deliberately outside that mask: Halo hides it through cutscenes,
         * so an empty HUD is ordinary and saying otherwise trains everyone to
         * ignore this line. Report it, do not fail on it. */
        unsigned required=lent.info.stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
        unsigned missing=empty&required,aged=stale&required;
        char note[96];note[0]=0;size_t n=0;
        if(empty&(1u<<HALO_PANORAMA_HUD_LAYER))n+=(size_t)snprintf(note,sizeof note," hud=empty");
        if(aged)n+=(size_t)snprintf(note+n,sizeof note-n," unchanged=0x%03X",aged);
        if(invalid)snprintf(note+n,sizeof note-n," invalid-projection=0x%03X",invalid);
        if(missing||invalid)
            fprintf(stderr,"[probe-gpu] frame=%llu slot=%d SPHERE INCOMPLETE empty=0x%03X%s (%s)\n",
                (unsigned long long)sequence,lent.slot,missing,note,detail);
        else
            fprintf(stderr,"[probe-gpu] frame=%llu slot=%d sphere complete%s (%s)\n",
                (unsigned long long)sequence,lent.slot,note,detail);
    }
}
/* HALO_PROBE_FIND_CURSOR=1: on the front-end menu, push the mouse right by a
 * known amount over a few frames, then left by the same, and report every
 * 16-bit, 32-bit and float field in the executable's data that moved with
 * it and moved back. This is how the UI cursor's position was located for
 * the gaze pointer. Diagnostic only: it reads guest memory and writes
 * nothing but the host's own mouse delta, which the original engine reads
 * through DirectInput exactly as it would a real mouse. */
static void probe_find_cursor(uint64_t sequence) {
    static int enabled=-1;
    if(enabled<0){const char *v=getenv("HALO_PROBE_FIND_CURSOR"); enabled=v&&!strcmp(v,"1");}
    if(!enabled||!engine_flat_base) return;
    enum { LO=0x00400000u, HI=0x00800000u, SPAN=HI-LO, PUSH=25, PUSH_FRAMES=4, SETTLE=16 };
    static uint8_t *a,*b,*c; static uint64_t base;
    extern int32_t host_mouse_dx;
    if(!base){ if(sequence<300) return; base=sequence; a=malloc(SPAN); memcpy(a,engine_flat_base+LO,SPAN);
        fprintf(stderr,"[cursor-find] snapshot A at frame %llu; pushing right %d over %d frames\n",(unsigned long long)sequence,PUSH*PUSH_FRAMES,PUSH_FRAMES); return; }
    uint64_t t=sequence-base;
    if(t>=1&&t<=PUSH_FRAMES) host_mouse_dx+=PUSH;
    else if(t==PUSH_FRAMES+SETTLE && !b){ b=malloc(SPAN); memcpy(b,engine_flat_base+LO,SPAN); fprintf(stderr,"[cursor-find] snapshot B; pushing left\n"); }
    else if(t>PUSH_FRAMES+SETTLE && t<=2*PUSH_FRAMES+SETTLE && b) host_mouse_dx-=PUSH;
    else if(t==2*PUSH_FRAMES+2*SETTLE && b && !c){
        c=malloc(SPAN); memcpy(c,engine_flat_base+LO,SPAN);
        long total=PUSH*PUSH_FRAMES; int shown=0;
        for(uint32_t off=0; off+4<=SPAN && shown<40; off+=2){
            int32_t A32,B32,C32; memcpy(&A32,a+off,4);memcpy(&B32,b+off,4);memcpy(&C32,c+off,4);
            int16_t A16,B16,C16; memcpy(&A16,a+off,2);memcpy(&B16,b+off,2);memcpy(&C16,c+off,2);
            long d32=(long)B32-A32, e32=(long)C32-B32, d16=(long)B16-A16, e16=(long)C16-B16;
            if((off%4)==0 && d32 && d32==-e32 && labs(d32)>=total/8 && labs(d32)<=total*8){
                fprintf(stderr,"[cursor-find] i32 %08x A=%d B=%d C=%d (moved %ld)\n",LO+off,A32,B32,C32,d32); shown++; continue; }
            if(d16 && d16==-e16 && labs(d16)>=total/8 && labs(d16)<=total*8){
                fprintf(stderr,"[cursor-find] i16 %08x A=%d B=%d C=%d (moved %ld)\n",LO+off,A16,B16,C16,d16); shown++; continue; }
            if((off%4)==0){ float Af,Bf,Cf; memcpy(&Af,a+off,4);memcpy(&Bf,b+off,4);memcpy(&Cf,c+off,4);
                if(isfinite(Af)&&isfinite(Bf)&&isfinite(Cf)){ float df=Bf-Af, ef=Cf-Bf;
                    if(fabsf(df)>=total/8.f && fabsf(df)<=total*8.f && fabsf(df+ef)<0.01f*fabsf(df)+0.001f){
                        fprintf(stderr,"[cursor-find] f32 %08x A=%g B=%g C=%g (moved %g)\n",LO+off,Af,Bf,Cf,df); shown++; } } }
        }
        fprintf(stderr,"[cursor-find] done: %d candidate(s)\n",shown);
    }
    /* Then push far right and far down to read the clamps, and print the
     * words round the X candidate so Y and the interface's extent show. */
    enum { FAR_START = 2*PUSH_FRAMES+2*SETTLE+4, FAR_FRAMES = 12, FAR = 400 };
    extern int32_t host_mouse_dy;
    if(t>FAR_START && t<=FAR_START+FAR_FRAMES){ host_mouse_dx+=FAR; host_mouse_dy+=FAR; }
    else if(t==FAR_START+FAR_FRAMES+SETTLE){
        fprintf(stderr,"[cursor-find] after pushing right and down by %d:",FAR*FAR_FRAMES);
        for(uint32_t addr=0x00718f70u; addr<=0x00718f90u; addr+=4){ int32_t v; memcpy(&v,engine_flat_base+addr,4); fprintf(stderr," %08x=%d",addr,v); }
        fprintf(stderr,"\n");
    }
    else if(t>FAR_START+FAR_FRAMES+SETTLE && t<=FAR_START+2*FAR_FRAMES+SETTLE){ host_mouse_dx-=FAR; host_mouse_dy-=FAR; }
    else if(t==FAR_START+2*FAR_FRAMES+2*SETTLE){
        fprintf(stderr,"[cursor-find] after pushing back left and up:");
        for(uint32_t addr=0x00718f70u; addr<=0x00718f90u; addr+=4){ int32_t v; memcpy(&v,engine_flat_base+addr,4); fprintf(stderr," %08x=%d",addr,v); }
        fprintf(stderr,"\n");
    }
}

/* HALO_PROBE_POINTER=u,v: stand in for the headset's gaze at one point of the
 * menu panel from frame 300, and report where the engine's cursor went. */
static void probe_pointer_test(uint64_t sequence) {
    static int enabled=-1; static float u,v;
    if(enabled<0){const char *s=getenv("HALO_PROBE_POINTER"); enabled=s&&sscanf(s,"%f,%f",&u,&v)==2;}
    if(!enabled||sequence<300) return;
    host_pointer_set(u,v,1,0);
    if(sequence<=318||sequence==330||sequence==360){ uint64_t frames=0; int x=0,y=0; host_pointer_stats(&frames,&x,&y);
        extern int32_t host_mouse_dx, host_mouse_dy; int32_t gx,gy; memcpy(&gx,engine_flat_base+0x00718f84u,4); memcpy(&gy,engine_flat_base+0x00718f88u,4);
        fprintf(stderr,"[pointer-test] frame=%llu servo-frames=%llu servo-read=(%d,%d) guest=(%d,%d) pending-delta=(%d,%d) target=(%d,%d)\n",(unsigned long long)sequence,(unsigned long long)frames,x,y,gx,gy,host_mouse_dx,host_mouse_dy,(int)lroundf(u*639.f),(int)lroundf(v*479.f)); }
}

/* HALO_PROBE_MOUSE_TRACE=dx,dy,frames: from frame 300 feed that mouse delta
 * every frame for `frames` frames, then the opposite for as many, printing
 * the engine's cursor each frame: how it responds to a delta per frame. */
static void probe_mouse_trace(uint64_t sequence) {
    static int enabled=-1; static int dx,dy,frames;
    if(enabled<0){const char *s=getenv("HALO_PROBE_MOUSE_TRACE"); enabled=s&&sscanf(s,"%d,%d,%d",&dx,&dy,&frames)==3&&frames>0&&frames<200;}
    if(!enabled||!engine_flat_base||sequence<300||sequence>300+2*(uint64_t)frames+4) return;
    extern int32_t host_mouse_dx, host_mouse_dy;
    uint64_t t=sequence-300; int32_t x,y; memcpy(&x,engine_flat_base+0x00718f84u,4); memcpy(&y,engine_flat_base+0x00718f88u,4);
    int sx=t<(uint64_t)frames?1:t<2*(uint64_t)frames?-1:0;
    fprintf(stderr,"[mouse-trace] frame=%llu cursor=(%d,%d) then feeding (%d,%d)\n",(unsigned long long)sequence,x,y,sx*dx,sx*dy);
    host_mouse_dx+=sx*dx; host_mouse_dy+=sx*dy;
}

/* HALO_PROBE_MOUSE_STAIR=1: from the middle of the panel feed deltas of
 * growing size, each followed by its opposite, printing the cursor after
 * each: the engine's mouse acceleration curve, count by count. */
static void probe_mouse_stair(uint64_t sequence) {
    static int enabled=-1;
    if(enabled<0){const char *s=getenv("HALO_PROBE_MOUSE_STAIR"); enabled=s&&!strcmp(s,"1");}
    if(!enabled||!engine_flat_base||sequence<300||sequence>300+70) return;
    extern int32_t host_mouse_dx, host_mouse_dy;
    uint64_t t=sequence-300; int32_t x,y; memcpy(&x,engine_flat_base+0x00718f84u,4); memcpy(&y,engine_flat_base+0x00718f88u,4);
    static const int sizes[]={1,2,4,6,8,12,16,24,32,48,64,96,128,192,256};
    int feed=0;
    if(t==0) feed=40;                                   /* off the corner first */
    else if(t>=2 && t<2+2*15){ int k=(int)(t-2)/2; feed=((t-2)%2==0)?sizes[k]:-sizes[k]; }
    fprintf(stderr,"[mouse-stair] frame=%llu cursor=(%d,%d) feeding dx=%d\n",(unsigned long long)sequence,x,y,feed);
    host_mouse_dx+=feed;
}

void menu_probe_present(void) {
    EngineVisionFrameInfo f={0};enginevision_frame_info(&f);
    capture_guest_pose(f.sequence);
    probe_panorama_gpu_census(f.sequence);
    probe_find_cursor(f.sequence);
    probe_pointer_test(f.sequence);
    probe_mouse_trace(f.sequence);
    probe_mouse_stair(f.sequence);
    /* Opt-in read-only publication timing; performs no framebuffer readback.
     * Every row covers the preceding 120 published frames, not display refresh. */
    static int timing_enabled=-1;
    static double timing_start;
    static uint64_t timing_frame;
    if(timing_enabled<0) {
        const char *value=getenv("HALO_PROBE_FRAME_TIMING");
        timing_enabled=value && !strcmp(value,"1");
    }
    if(timing_enabled && (!timing_frame || f.sequence-timing_frame>=120)) {
        double now=probe_monotonic();
        EngineVisionPanoramaInfo pano={0};enginevision_panorama_info(&pano);
        if(timing_frame)fprintf(stderr,"[probe-timing] frame=%llu previous=%llu seconds=%.9f shell=%u tick=%u panorama=%llu epoch=%llu status=%u failure=%u\n",
            f.sequence,timing_frame,now-timing_start,guest_pose.shell,guest_pose.tick,
            pano.sequence,pano.source_epoch,pano.status,pano.failure_reason);
        /* The same split the headset reports, as deltas over this window, so
         * the Mac can say how often the game sleeps and yields per frame. */
        { static uint64_t last[12]; uint64_t now_v[12]={0}; extern void host_draw_profile_snapshot(uint64_t out[12]);
          host_draw_profile_snapshot(now_v);
          if(timing_frame){ uint64_t frames=f.sequence-timing_frame;
            EngineVisionDrawProfile budget={0}; enginevision_draw_profile(&budget);
            static uint64_t last_cpu_ns;
            fprintf(stderr,"[probe-cpu] frame=%llu cpuTotalNS=%llu cpuDeltaNS=%llu fast=%d crcFast=%d\n",
              f.sequence,(unsigned long long)budget.engine_cpu_ns,
              (unsigned long long)(budget.engine_cpu_ns-last_cpu_ns),mr_fast_paths_enabled(),host_texture_crc_fast_enabled());
            last_cpu_ns=budget.engine_cpu_ns;
            fprintf(stderr,"[probe-split] frames=%llu passes=%llu pass=%.3fs readback=%.3fs/%llu upload=%.3fs sleep=%.3fs/%llu yields=%llu (%.1f/frame) spin=%.3fs budget=%u/2 busy=%.1fms onsets=%llu\n",
              (unsigned long long)frames,(unsigned long long)(now_v[0]-last[0]),(now_v[1]-last[1])/1e9,(now_v[2]-last[2])/1e9,
              (unsigned long long)(now_v[3]-last[3]),(now_v[7]-last[7])/1e9,(now_v[9]-last[9])/1e9,(unsigned long long)(now_v[8]-last[8]),
              (unsigned long long)(now_v[10]-last[10]),frames?(double)(now_v[10]-last[10])/frames:0.0,(now_v[11]-last[11])/1e9,
              budget.panorama_extra_half,budget.panorama_busy_seconds*1000.f,(unsigned long long)budget.haptic_onsets); }
          memcpy(last,now_v,sizeof last); }
        if(timing_frame){extern void host_draw_profile_report(unsigned,double);
            host_draw_profile_report((unsigned)f.sequence,now-timing_start);}
        /* Optional within-process ABBA comparison after map entry. Identical
         * quality and view budget; report the completed window before changing
         * the next one's renderer. Repeated blocks reduce temporal load bias. */
        const char *draw_ab=getenv("HALO_PROBE_RENDER_AB");
        if(draw_ab && !strcmp(draw_ab,"1") && f.sequence>=1441) {
            unsigned phase=(unsigned)((f.sequence-1441)/240)%4;
            int next=phase==1 || phase==2;
            if(next!=mr_fast_paths_enabled())mr_set_fast_paths(next);
            host_texture_crc_set_fast(next);
        }
        timing_frame=f.sequence;timing_start=now;
    }
    probe_snapshot_present(f.sequence);
    // Diagnostic census only: original datum array (12-byte entries), with
    // tag names resolved through the same 32-byte table used by 0050EE20.
    if(getenv("HALO_OBJECT_CENSUS") && f.sequence>=450 && f.sequence%120==0) {
        uint32_t table=guest_u32(0x008603B0),tags=guest_u32(0x0087BC14);
        if(table && tags) {
            uint32_t entries=guest_u32(table+0x34); uint16_t count;
            memcpy(&count,engine_flat_base+table+0x20,2);
            if(entries && count<=4096) for(unsigned i=0;i<count;i++) {
                uint32_t entry=entries+12*i,object=guest_u32(entry+8);
                uint16_t salt;memcpy(&salt,engine_flat_base+entry,2);
                if(!salt || !object || object>UINT32_MAX-0x1000)continue;
                uint32_t definition=guest_u32(object),tag=tags+32*(definition&0xffff);
                uint32_t name=guest_u32(tag+0x10),data=guest_u32(tag+0x14);
                char text[161]={0};if(name && name<=UINT32_MAX-160)memcpy(text,engine_flat_base+name,160);
                float pos[3];memcpy(pos,engine_flat_base+object+0x5c,12);
                fprintf(stderr,"[object] frame=%llu datum=%04x%04x flags=%08x def=%08x model=%08x pos=%.2f,%.2f,%.2f name=%s\n",f.sequence,salt,i,guest_u32(object+0x10),definition,data?guest_u32(data+0x34):0,pos[0],pos[1],pos[2],text);
            }
        }
    }
    if(!getenv("HALO_KEYSEQ")) {
        bool scripted=!atomic_load(&probe_live_control);
        host_keyboard_state[0xD0] = scripted && ((probe_mode == 3 && f.sequence >= 8 && f.sequence < 12) || (probe_mode == 8 && f.sequence >= 125 && f.sequence < 126)) ? 0x80 : 0;
        host_keyboard_state[0x1C] = scripted && probe_mode == 4 && f.sequence >= 8 && f.sequence < 12 ? 0x80 : 0;
    }
    if(f.sequence<=3 || (f.sequence>=8 && f.sequence<=15) || f.sequence==30) {
        fprintf(stderr,"[probe] input frame=%llu active=%u count=%u blocked=%u shell=%u device=%08x assignment=%08x\n",f.sequence,engine_flat_base[0x006B15F8],guest_u32(0x006B1844),engine_flat_base[0x007196D8],engine_flat_base[0x00718FC9],guest_u32(0x006B1848),guest_u32(0x006B1A98));
    }
}


int main(int argc, char **argv) {
    if(argc != 3)return 2;
    const char *input_file=getenv("HALO_PROBE_INPUT_FILE");
    double live_seconds=1800;
    uint64_t live_from_frame=0;
    if(input_file) {
        if(!*input_file || getenv("HALO_KEYSEQ")) {
            fprintf(stderr,"[probe] live input requires a nonempty file path and no HALO_KEYSEQ\n");return 2;
        }
        probe_input_file=[NSString stringWithUTF8String:input_file];
        if(!probe_input_file) return 2;
        const char *from_frame=getenv("HALO_PROBE_INPUT_FROM_FRAME");
        if(from_frame) {
            char *end=0; long value=strtol(from_frame,&end,10);
            if(end==from_frame || *end || value<0 || value>INT_MAX) {
                fprintf(stderr,"[probe] HALO_PROBE_INPUT_FROM_FRAME must be in [0,%d]\n",INT_MAX);return 2;
            }
            live_from_frame=(uint64_t)value;
        }
        const char *seconds=getenv("HALO_PROBE_LIVE_SECONDS");
        if(seconds) {
            char *end=0; live_seconds=strtod(seconds,&end);
            if(end==seconds || *end || !isfinite(live_seconds) || live_seconds<1 || live_seconds>3600) {
                fprintf(stderr,"[probe] HALO_PROBE_LIVE_SECONDS must be in [1,3600]\n");return 2;
            }
        }
        fprintf(stderr,"[live-input] file=%s session_seconds=%.0f from_frame=%llu\n",input_file,live_seconds,live_from_frame);
    }
    probe_mode=atoi(argv[2]); host_frame_limit=probe_mode==9 ? 6000 : probe_mode>=7 ? 160 : probe_mode>=5 ? 120 : 30;
    if(probe_input_file) host_frame_limit=INT_MAX;
    const char *frame_limit=getenv("HALO_PROBE_FRAMES");
    if((probe_mode==9 || probe_input_file) && frame_limit) {
        char *end=0; long requested=strtol(frame_limit,&end,10);
        if(end==frame_limit || *end || requested<=0 || requested>INT_MAX) {
            fprintf(stderr,"[probe] HALO_PROBE_FRAMES must be an integer in [1,%d]\n",INT_MAX);return 2;
        }
        host_frame_limit=(int)requested;
        fprintf(stderr,"[probe] launch frame limit=%d\n",host_frame_limit);
    }
    HostGCSnapshot pad={0}; pad.connected=true; pad.sequence=1;
    hostgc_inject_test_snapshot(&pad);
    /* Rendering-only comparisons can exclude an unavailable Mac audio device.
     * Suspend before DirectSound creates its queue, using the normal lifecycle
     * API. This opt-in belongs only to the diagnostic executable; it is never
     * a shipped app setting or evidence of audible playback. */
    const char *suspend_audio=getenv("HALO_PROBE_SUSPEND_AUDIO");
    if(suspend_audio && !strcmp(suspend_audio,"1")) {
        host_dsound_pause_output();
        fprintf(stderr,"[probe] audio output deliberately suspended; rendering-only measurement\n");
    }
    int error=enginevision_start(argv[1]); if(error)return error;
    uint64_t previous=0;
    double session_deadline=probe_monotonic()+live_seconds, next_poll=0, next_audio_poll=0;
    const char *audio_trace=getenv("HALO_AUDIO_VOICE_TRACE");
    bool poll_audio=audio_trace && *audio_trace && strcmp(audio_trace,"0");
    for(int i=0;probe_input_file ? probe_monotonic()<session_deadline : i<60000;i++) {
        @autoreleasepool {
            EngineVisionFrameInfo f={0}; enginevision_frame_info(&f);
            if(poll_audio && probe_monotonic()>=next_audio_poll){
                // Poll outside the render/audio callbacks, even when Present stalls.
                host_dsound_get_stats(NULL,NULL,NULL);
                next_audio_poll=probe_monotonic()+0.25;
            }
            if(probe_input_file && !atomic_load(&probe_live_control) && f.sequence>=live_from_frame) {
                atomic_store(&probe_live_control,true);
                live_injection_sequence=pad.sequence; live_active=true; live_neutral();
                fprintf(stderr,"[live-input] exclusive control begins at frame=%llu\n",f.sequence);
            }
            // Run independently of new engine frames so a stalled renderer
            // cannot leave held controls behind. File polling is capped at 50 Hz.
            if(atomic_load(&probe_live_control)) {
                double now=probe_monotonic();
                if(now>=next_poll) { live_poll(now,probe_wall_ms()); next_poll=now+0.02; }
            }
            if(f.sequence!=previous) {
                previous=f.sequence;
                if(!atomic_load(&probe_live_control)) {
                pad.sequence++;
                pad.dpad_down=(probe_mode==1 && f.sequence>=8 && f.sequence<12) || (probe_mode==7 && f.sequence>=125 && f.sequence<126);
                pad.buttons[HOSTGC_BTN_DPAD_DOWN]=pad.dpad_down;
                int skip9 = probe_mode==9 && ((f.sequence>=400&&f.sequence<405)||(f.sequence>=800&&f.sequence<805)||(f.sequence>=1200&&f.sequence<1205)||(f.sequence>=1600&&f.sequence<1605)||(f.sequence>=2000&&f.sequence<2005)||(f.sequence>=2600&&f.sequence<2605)||(f.sequence>=3200&&f.sequence<3205)||(f.sequence>=3800&&f.sequence<3805));
                pad.buttons[HOSTGC_BTN_A]=((probe_mode==2 || probe_mode>=5) && f.sequence>=8 && f.sequence<12) || (probe_mode>=6 && f.sequence>=90 && f.sequence<94) || (probe_mode==9 && f.sequence>=150 && f.sequence<154) || skip9;
                pad.buttons[HOSTGC_BTN_MENU]=skip9 && f.sequence<1000; /* skip cinematic only; never pause gameplay */
                pad.buttons[HOSTGC_BTN_B]=probe_mode==5 && f.sequence>=90 && f.sequence<94;
                static int noprobe=-1; static const char*padseq=0; static const char*stickseq=0; if(noprobe<0){ padseq=getenv("HALO_PADSEQ"); stickseq=getenv("HALO_STICKSEQ"); noprobe=(getenv("HALO_KEYSEQ")||getenv("HALO_NO_PROBE_INPUT")||padseq||stickseq)?1:0; }
                if(noprobe){ pad.dpad_down=0; pad.dpad_up=0; pad.dpad_left=0; pad.dpad_right=0; pad.lx=0; pad.ly=0; pad.rx=0; pad.ry=0; pad.lt=0; pad.rt=0;
                    for(int b=0;b<HOSTGC_BUTTON_COUNT;b++) pad.buttons[b]=0;
                    if(padseq){ const char*p=padseq; while(*p){ int a=0,z=0; char bc=0; if(sscanf(p,"%d:%d:%c",&a,&z,&bc)==3 && (long long)f.sequence>=a && (long long)f.sequence<=z){
                        switch(bc){ case 'A': pad.buttons[HOSTGC_BTN_A]=1; break; case 'B': pad.buttons[HOSTGC_BTN_B]=1; break; case 'X': pad.buttons[HOSTGC_BTN_X]=1; break; case 'Y': pad.buttons[HOSTGC_BTN_Y]=1; break; case 'M': pad.buttons[HOSTGC_BTN_MENU]=1; break;
                          case '1': pad.buttons[HOSTGC_BTN_LSHOULDER]=1; break; case '2': pad.buttons[HOSTGC_BTN_RSHOULDER]=1; break;
                          case '3': pad.buttons[HOSTGC_BTN_LTRIGGER]=1; pad.lt=1; break; case '4': pad.buttons[HOSTGC_BTN_RTRIGGER]=1; pad.rt=1; break;
                          case '5': pad.buttons[HOSTGC_BTN_LTHUMB]=1; break; case '6': pad.buttons[HOSTGC_BTN_RTHUMB]=1; break; case 'C': pad.buttons[HOSTGC_BTN_OPTIONS]=1; break;
                          case 'D': pad.dpad_down=1; pad.buttons[HOSTGC_BTN_DPAD_DOWN]=1; pad.ly=-1.0f; break; case 'U': pad.dpad_up=1; pad.buttons[HOSTGC_BTN_DPAD_UP]=1; pad.ly=1.0f; break;
                          case 'L': pad.dpad_left=1; pad.lx=-1.0f; break; case 'R': pad.dpad_right=1; pad.lx=1.0f; break; } }
                      const char*c=strchr(p,','); if(!c) break; p=c+1; } } }
                if(stickseq) {
                    const char *p=stickseq;
                    while(*p) {
                        long long start=0,end=0; float lx=0,ly=0,rx=0,ry=0;
                        if(sscanf(p,"%lld:%lld:%f:%f:%f:%f",&start,&end,&lx,&ly,&rx,&ry)==6 &&
                           (long long)f.sequence>=start && (long long)f.sequence<=end &&
                           isfinite(lx) && isfinite(ly) && isfinite(rx) && isfinite(ry)) {
                            pad.lx=fmaxf(-1,fminf(1,lx)); pad.ly=fmaxf(-1,fminf(1,ly));
                            pad.rx=fmaxf(-1,fminf(1,rx)); pad.ry=fmaxf(-1,fminf(1,ry));
                        }
                        const char *comma=strchr(p,','); if(!comma)break; p=comma+1;
                    }
                    static float old_lx=0,old_ly=0,old_rx=0,old_ry=0;
                    if(pad.lx!=old_lx || pad.ly!=old_ly || pad.rx!=old_rx || pad.ry!=old_ry) {
                        fprintf(stderr,"[probe] stick frame=%llu lx=%.3f ly=%.3f rx=%.3f ry=%.3f\n",f.sequence,pad.lx,pad.ly,pad.rx,pad.ry);
                        old_lx=pad.lx; old_ly=pad.ly; old_rx=pad.rx; old_ry=pad.ry;
                    }
                }
                hostgc_inject_test_snapshot(&pad);
                if(f.sequence>=6 && f.sequence<=15) fprintf(stderr,"[probe] frame=%llu padDown=%d cross=%d\n",f.sequence,pad.dpad_down,pad.buttons[HOSTGC_BTN_A]);
                }
                if(probe_mode==9 && (f.sequence%20==0)) { char st[512];enginevision_copy_status(st,sizeof st); fprintf(stderr,"[launch] rframe=%llu shell=%u active=%u count=%u blocked=%u status=%s\n", f.sequence, engine_flat_base[0x00718FC9], engine_flat_base[0x006B15F8], guest_u32(0x006B1844), engine_flat_base[0x007196D8], st); }
            }
            if(enginevision_runtime_state()>=ENGINEVISION_STOPPED) {
                uint64_t heap[6];host_heap_stats(heap);
                fprintf(stderr,"[probe-heap] live=%llu peak=%llu turnover=%llu allocations=%llu metadata=%llu highwater=%llu\n",
                    (unsigned long long)heap[0],(unsigned long long)heap[1],(unsigned long long)heap[2],
                    (unsigned long long)heap[3],(unsigned long long)heap[4],(unsigned long long)heap[5]);
                char status[512];enginevision_copy_status(status,sizeof status);
                { uint64_t hits=0,uploads=0,evictions=0; mr_texture_cache_counters(&hits,&uploads,&evictions);
                  fprintf(stderr,"[probe-texture-cache] hits=%llu uploads=%llu evictions=%llu\n",(unsigned long long)hits,(unsigned long long)uploads,(unsigned long long)evictions); }
                fprintf(stderr,"[probe] done state=%d exit=%d frames=%llu joystickReads=%llu keyboardEvents=%llu status=%s\n",enginevision_runtime_state(),enginevision_exit_code(),f.sequence,host_dinput_gamepad_reads(),host_dinput_keyboard_events(),status);
                live_neutral();
                return enginevision_exit_code();
            }
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
    }
    if(probe_input_file) { live_neutral(); enginevision_request_stop(); }
    fprintf(stderr,"[probe] timeout\n");return 124;
}
