// HTTP(S) fetcher for macOS/iOS: NSURLSession data task streamed into the file (resume with Range).
#import <Foundation/Foundation.h>

#include <cstdio>

#include "ThreadDownload.h"

namespace rts::pack {

std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string& destPath, std::string& error) {
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!u) { error = "bad URL " + url; return nullptr; }
    auto dl = std::make_unique<ThreadDownload>();
    dl->run([u, destPath](ThreadDownload& d) {
        @autoreleasepool {
            uint64_t have = 0;
            if (std::FILE* f = std::fopen(destPath.c_str(), "rb")) { std::fseek(f, 0, SEEK_END); have = uint64_t(std::ftell(f)); std::fclose(f); }
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:60];
            if (have) [req setValue:[NSString stringWithFormat:@"bytes=%llu-", (unsigned long long)have] forHTTPHeaderField:@"Range"];
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block NSURL* tmp = nil;
            __block NSInteger code = 0;
            __block NSString* err = nil;
            // downloadTask writes to a temp file without holding the chunk in memory.
            NSURLSessionDownloadTask* task = [[NSURLSession sharedSession] downloadTaskWithRequest:req
                completionHandler:^(NSURL* location, NSURLResponse* resp, NSError* e) {
                    if (e) err = e.localizedDescription;
                    code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)resp).statusCode : 0;
                    if (location) {   // must be moved before the handler returns
                        tmp = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]]];
                        [[NSFileManager defaultManager] moveItemAtURL:location toURL:tmp error:nil];
                    }
                    dispatch_semaphore_signal(sem);
                }];
            [task resume];
            while (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0)
                d.bytes_ = have + uint64_t(task.countOfBytesReceived > 0 ? task.countOfBytesReceived : 0);
            if (err) { d.fail(err.UTF8String); return; }
            if (code == 416) { d.succeed(); return; }
            if (code != 200 && code != 206) { d.fail("HTTP " + std::to_string(long(code))); return; }
            if (code == 200) have = 0;
            std::FILE* out = std::fopen(destPath.c_str(), have ? "ab" : "wb");
            std::FILE* in = tmp ? std::fopen(tmp.fileSystemRepresentation, "rb") : nullptr;
            bool ok = out && in;
            char buf[1 << 16];
            for (size_t n; ok && (n = std::fread(buf, 1, sizeof buf, in)) > 0;) ok = std::fwrite(buf, 1, n, out) == n;
            if (in) std::fclose(in);
            if (out) std::fclose(out);
            if (tmp) [[NSFileManager defaultManager] removeItemAtURL:tmp error:nil];
            if (!ok) { d.fail("cannot write " + destPath); return; }
            d.succeed();
        }
    });
    return dl;
}

void setAndroidJavaVm(void*) {}

} // namespace rts::pack
