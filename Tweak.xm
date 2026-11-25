// MusicRemote v0.1. A JSON bridge injected into Music.app. It reads one
// request file, drives MPMusicPlayerController's system player, and writes
// one response file. Darwin notifications signal both directions. The wire
// contract (paths, actions, response fields) lives in PROTOCOL.md. MediaPlayer
// only; the one private call is MPMediaLibrary's liked-state setter, since the
// SDK has no public way to set it.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <MediaPlayer/MediaPlayer.h>
#import <stdio.h>
#import <unistd.h>

static NSString *const kMusicRemoteDirectory = @"/var/mobile/Media/.musicremote";
static NSString *const kMusicRemoteRequestPath = @"/var/mobile/Media/.musicremote/req.json";
static NSString *const kMusicRemoteResponsePath = @"/var/mobile/Media/.musicremote/res.json";
static NSString *const kMusicRemoteRequestNote = @"com.chris.musicremote.req";
static NSString *const kMusicRemoteResponseNote = @"com.chris.musicremote.res";

// Not in the iOS 16 SDK headers, but present on iOS 15. The
// respondsToSelector() checks below keep a missing one from crashing.
@interface MPMusicPlayerController (MusicRemoteLegacy)
@property (nonatomic, readonly) NSUInteger numberOfItems;
@property (nonatomic, readonly) NSTimeInterval playbackDuration;
@end

@interface MPMediaLibrary (MusicRemoteLegacy)
// No public API sets the loved flag; this is the private model-layer setter the
// Music app itself goes through. Guarded by respondsToSelector before use.
- (void)setValues:(NSArray *)values forProperties:(NSArray *)properties
    forItemPersistentIDs:(NSArray *)persistentIDs;
@end

// Lowercase, fold diacritics and width, collapse whitespace, trim.
static NSString *MRNormalize(NSString *text) {
    if (text == nil) return @"";
    NSString *lowered = [[text stringByFoldingWithOptions:(NSDiacriticInsensitiveSearch | NSWidthInsensitiveSearch) locale:nil] lowercaseString];
    NSArray<NSString *> *words = [lowered componentsSeparatedByCharactersInSet:
                                  [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSMutableArray<NSString *> *kept = [NSMutableArray array];
    for (NSString *word in words) {
        if (word.length > 0) [kept addObject:word];
    }
    return [kept componentsJoinedByString:@" "];
}
static BOOL MRIsExact(NSString *text, NSString *query) {
    return [MRNormalize(text) isEqualToString:MRNormalize(query)];
}
static BOOL MRContains(NSString *text, NSString *query) {
    if (MRNormalize(query).length == 0) return NO;
    return [MRNormalize(text) rangeOfString:MRNormalize(query)].location != NSNotFound;
}
static NSString *MRItemString(MPMediaItem *item, NSString *property) {
    id value = [item valueForProperty:property];
    return [value isKindOfClass:[NSString class]] ? value : @"";
}
// Persistent IDs are 64-bit and would lose precision in JSON numbers.
static NSString *MRIDString(MPMediaItem *item, NSString *property) {
    id value = [item valueForProperty:property];
    if (![value isKindOfClass:[NSNumber class]]) return @"";
    return [NSString stringWithFormat:@"%llu", (unsigned long long)[value unsignedLongLongValue]];
}
static NSString *MRStringArgument(NSDictionary *args, NSString *key) {
    id value = args[key];
    return [value isKindOfClass:[NSString class]] ? value : nil;
}
static NSString *MRQueryArgument(NSDictionary *args) {
    NSString *query = MRStringArgument(args, @"query");
    if (MRNormalize(query).length == 0) {
        @throw [NSException exceptionWithName:@"MusicRemoteError" reason:@"missing \"query\"" userInfo:nil];
    }
    return query;
}
static NSString *MRModeArgument(NSDictionary *args) {
    NSString *mode = MRStringArgument(args, @"mode");
    return mode.length > 0 ? mode : @"replace";
}
static void MRFail(NSString *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    NSString *reason = [[NSString alloc] initWithFormat:format arguments:arguments];
    va_end(arguments);
    @throw [NSException exceptionWithName:@"MusicRemoteError" reason:reason userInfo:nil];
}
static void MREnsureDirectory(void) {
    NSError *error = nil;
    BOOL created = [[NSFileManager defaultManager] createDirectoryAtPath:kMusicRemoteDirectory
                                             withIntermediateDirectories:YES attributes:nil error:&error];
    if (!created) NSLog(@"[MusicRemote] cannot create %@: %@", kMusicRemoteDirectory, error.localizedDescription);
}
static NSString *MRStateString(MPMusicPlaybackState state) {    switch (state) {
        case MPMusicPlaybackStatePlaying: return @"playing";
        case MPMusicPlaybackStatePaused: return @"paused";
        case MPMusicPlaybackStateStopped: return @"stopped";
        case MPMusicPlaybackStateInterrupted: return @"interrupted";
        case MPMusicPlaybackStateSeekingForward:
        case MPMusicPlaybackStateSeekingBackward: return @"seeking";
        default: return @"unknown";
    }
}
static NSString *MRAlbumTitleForCollection(MPMediaItemCollection *collection) {
    MPMediaItem *item = collection.representativeItem ?: collection.items.firstObject;
    return item ? MRItemString(item, MPMediaItemPropertyAlbumTitle) : @"";
}
static NSArray<NSString *> *MRFirstUniqueTitles(NSArray<NSString *> *titles, NSUInteger limit) {
    NSMutableArray<NSString *> *result = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (NSString *title in titles) {
        NSString *key = MRNormalize(title);
        if (key.length == 0 || [seen containsObject:key]) continue;
        [seen addObject:key];
        [result addObject:title];
        if (result.count >= limit) break;
    }
    return result;
}
// For the "no album matched" error. Falls back to a songs query when the
// albums query is empty, which happens with local-only libraries.
static NSArray<NSString *> *MRNearbyAlbumTitles(NSArray<NSString *> *titles) {
    NSMutableArray<NSString *> *source = [titles mutableCopy];
    if (source.count == 0) {
        for (MPMediaItem *song in [MPMediaQuery songsQuery].items ?: @[]) {
            [source addObject:MRItemString(song, MPMediaItemPropertyAlbumTitle)];
            if (source.count >= 30) break;
        }
    }
    return MRFirstUniqueTitles(source, 3);
}
static NSArray<MPMediaItem *> *MRSortedSongs(NSArray<MPMediaItem *> *songs) {
    return [songs sortedArrayUsingComparator:^NSComparisonResult(MPMediaItem *a, MPMediaItem *b) {
        NSComparisonResult order = [MRItemString(a, MPMediaItemPropertyAlbumTitle)
            localizedCaseInsensitiveCompare:MRItemString(b, MPMediaItemPropertyAlbumTitle)];
        if (order != NSOrderedSame) return order;
        if (a.discNumber != b.discNumber) {
            return a.discNumber < b.discNumber ? NSOrderedAscending : NSOrderedDescending;
        }
        if (a.albumTrackNumber != b.albumTrackNumber) {
            return a.albumTrackNumber < b.albumTrackNumber ? NSOrderedAscending : NSOrderedDescending;
        }
        return NSOrderedSame;
    }];
}
// MPMusicShuffleModeOff is 1, which reads like "on" in raw JSON, so name it.
static NSString *MRShuffleString(MPMusicShuffleMode mode) {
    switch (mode) {
        case MPMusicShuffleModeOff: return @"off";
        case MPMusicShuffleModeSongs: return @"songs";
        case MPMusicShuffleModeAlbums: return @"albums";
        default: return @"default";
    }
}

static NSDictionary *MRStatusDict(void) {
    MPMusicPlayerController *player = [MPMusicPlayerController systemMusicPlayer];
    MPMediaItem *item = player.nowPlayingItem;
    NSMutableDictionary *data = [NSMutableDictionary dictionary];
    data[@"state"] = MRStateString(player.playbackState);
    data[@"shuffle"] = MRShuffleString(player.shuffleMode);
    data[@"title"] = item ? MRItemString(item, MPMediaItemPropertyTitle) : @"";
    data[@"artist"] = item ? MRItemString(item, MPMediaItemPropertyArtist) : @"";
    data[@"album"] = item ? MRItemString(item, MPMediaItemPropertyAlbumTitle) : @"";
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSUInteger index = player.indexOfNowPlayingItem;
    NSUInteger total = [player respondsToSelector:@selector(numberOfItems)] ? player.numberOfItems : 0;
    NSTimeInterval duration = [player respondsToSelector:@selector(playbackDuration)] ? player.playbackDuration : 0.0;
    data[@"elapsed"] = @(player.currentPlaybackTime);
    data[@"index"] = @(index == NSNotFound ? -1 : (NSInteger)index);
    data[@"total"] = @(total);
    data[@"duration"] = @(duration);
#pragma clang diagnostic pop
    // 64-bit IDs travel as strings so JSON never rounds them.
    if (item != nil) {
        data[@"albumID"] = MRIDString(item, MPMediaItemPropertyAlbumPersistentID);
        data[@"albumArtistID"] = MRIDString(item, MPMediaItemPropertyAlbumArtistPersistentID);
        data[@"trackID"] = MRIDString(item, MPMediaItemPropertyPersistentID);
        data[@"disc"] = @(item.discNumber);
        data[@"track"] = @(item.albumTrackNumber);
    }
    return data;
}

@interface MRBridge : NSObject
+ (instancetype)sharedBridge;
- (void)start;
- (dispatch_queue_t)workQueue;
@end

@interface MRBridge ()
@property (nonatomic) dispatch_queue_t workQueue;
- (void)installDarwinObserver;
- (void)writeMarker:(NSString *)name;
- (void)trace:(NSString *)format, ...;
- (void)applicationDidBecomeActive:(NSNotification *)notification;
- (void)processRequest;
- (void)sendResponse:(NSDictionary *)response;
- (NSDictionary *)dispatchAction:(NSString *)action args:(NSDictionary *)args;
- (NSDictionary *)queueAlbum:(NSDictionary *)args;
- (NSDictionary *)queueArtist:(NSDictionary *)args;
- (NSDictionary *)queueSong:(NSDictionary *)args;
- (NSDictionary *)queueByID:(NSDictionary *)args;
- (NSDictionary *)queueByStore:(NSDictionary *)args;
- (NSDictionary *)searchSongs:(NSDictionary *)args;
- (NSDictionary *)libraryStats;
- (NSDictionary *)volumeAction:(NSDictionary *)args;
- (NSDictionary *)like:(NSDictionary *)args;
- (NSDictionary *)finishQueue:(NSArray<MPMediaItem *> *)songs mode:(NSString *)mode
                      matched:(NSString *)matched others:(NSArray<NSString *> *)others;
@end

static void MRRequestCallback(CFNotificationCenterRef center, void *observer, CFNotificationName name,
                              const void *object, CFDictionaryRef userInfo) {
    (void)center; (void)name; (void)object; (void)userInfo;
    MRBridge *bridge = (__bridge MRBridge *)observer;
    [bridge writeMarker:@"notified.txt"];
    dispatch_async([bridge workQueue], ^{
        [bridge processRequest];
    });
}

@implementation MRBridge

+ (instancetype)sharedBridge {
    static MRBridge *bridge = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        bridge = [[MRBridge alloc] init];
        // All player calls run here, never on Music.app's main thread: calling
        // the system player from the main thread deadlocks against the app.
        bridge.workQueue = dispatch_queue_create("com.chris.musicremote.work", DISPATCH_QUEUE_SERIAL);
    });
    return bridge;
}

- (void)start {
    MREnsureDirectory();
    [self writeMarker:@"loaded.txt"];
    [self installDarwinObserver];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(applicationDidBecomeActive:)
                                                 name:UIApplicationDidBecomeActiveNotification
                                               object:nil];
    NSLog(@"[MusicRemote] bridge started");
    // Touch the system player once so the first real command does not pay the
    // multi-second cold start of mediaserverd's now-playing handshake.
    dispatch_async([self workQueue], ^{
        (void)[MPMusicPlayerController systemMusicPlayer].playbackState;
        NSLog(@"[MusicRemote] player warmed");
    });
    // The CLI posts before it launches us when Music.app was closed, so the
    // notification is already gone by now. Pick up any request left waiting.
    if ([[NSFileManager defaultManager] fileExistsAtPath:kMusicRemoteRequestPath]) {
        dispatch_async([self workQueue], ^{
            [self processRequest];
        });
    }
}

// Diagnostic breadcrumb: proves the dylib loaded and a notification arrived.
- (void)writeMarker:(NSString *)name {
    NSString *text = [NSString stringWithFormat:@"%@ %.0f", name, [NSDate date].timeIntervalSince1970];
    NSString *path = [kMusicRemoteDirectory stringByAppendingPathComponent:name];
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

// Append-only breadcrumb trail for the request path.
- (void)trace:(NSString *)format, ... {
    va_list arguments;
    va_start(arguments, format);
    NSString *line = [[NSString alloc] initWithFormat:format arguments:arguments];
    va_end(arguments);
    NSString *path = [kMusicRemoteDirectory stringByAppendingPathComponent:@"trace.log"];
    NSString *existing = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSString *text = [existing stringByAppendingFormat:@"%.1f %@\n", [NSDate date].timeIntervalSince1970, line];
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

// Remove first so a re-arm cannot register the callback twice.
- (void)installDarwinObserver {
    CFNotificationCenterRef center = CFNotificationCenterGetDarwinNotifyCenter();
    CFNotificationCenterRemoveEveryObserver(center, (__bridge const void *)self);
    CFNotificationCenterAddObserver(center, (__bridge const void *)self, MRRequestCallback,
                                    (__bridge CFStringRef)kMusicRemoteRequestNote, NULL,
                                    CFNotificationSuspensionBehaviorDeliverImmediately);
}

// Darwin notifications are dropped while suspended, so re-register on wake.
- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    MREnsureDirectory();
    [self installDarwinObserver];
    NSLog(@"[MusicRemote] re-armed after activation");
}

- (void)processRequest {
    NSData *requestData = [NSData dataWithContentsOfFile:kMusicRemoteRequestPath];
    [self trace:@"process reqData=%lu", (unsigned long)requestData.length];
    if (requestData == nil) return;
    // Unlink before acting, so a repeat notification cannot replay this command.
    unlink([kMusicRemoteRequestPath fileSystemRepresentation]);

    id parsed = [NSJSONSerialization JSONObjectWithData:requestData options:0 error:nil];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        NSLog(@"[MusicRemote] request is not a JSON object");
        [self sendResponse:@{@"id": @"", @"ok": @NO, @"error": @"request is not a JSON object"}];
        return;
    }

    NSDictionary *request = parsed;
    NSString *requestID = [request[@"id"] isKindOfClass:[NSString class]] ? request[@"id"] : @"";
    NSString *action = [request[@"action"] isKindOfClass:[NSString class]] ? request[@"action"] : @"";
    NSDictionary *args = [request[@"args"] isKindOfClass:[NSDictionary class]] ? request[@"args"] : @{};
    NSLog(@"[MusicRemote] request %@ action=%@", requestID, action);

    NSDictionary *response = nil;
    @try {
        [self trace:@"dispatch %@", action];
        NSDictionary *data = [self dispatchAction:action args:args];
        [self trace:@"dispatch done %@", action];
        response = @{@"id": requestID, @"ok": @YES, @"data": data ?: @{}};
    } @catch (NSException *exception) {
        [self trace:@"exception %@", exception.reason];
        response = @{@"id": requestID, @"ok": @NO, @"error": exception.reason ?: @"unknown error"};
    }
    [self sendResponse:response];
}

- (void)sendResponse:(NSDictionary *)response {
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:response options:0 error:&error];
    if (data == nil) {
        NSLog(@"[MusicRemote] cannot encode response: %@", error.localizedDescription);
        return;
    }
    MREnsureDirectory();
    NSString *temporaryPath = [kMusicRemoteResponsePath stringByAppendingString:@".tmp"];
    if (![data writeToFile:temporaryPath atomically:NO]) {
        [self trace:@"response write failed %@", temporaryPath];
        return;
    }
    if (rename([temporaryPath fileSystemRepresentation],
               [kMusicRemoteResponsePath fileSystemRepresentation]) != 0) {
        [self trace:@"response rename failed"];
        return;
    }
    [self trace:@"response written ok=%@", response[@"ok"]];

    CFNotificationCenterPostNotificationWithOptions(CFNotificationCenterGetDarwinNotifyCenter(),
                                                    (__bridge CFStringRef)kMusicRemoteResponseNote,
                                                    NULL, NULL, kCFNotificationDeliverImmediately);
    NSLog(@"[MusicRemote] response %@ ok=%@", response[@"id"], response[@"ok"]);
}

- (NSDictionary *)dispatchAction:(NSString *)action args:(NSDictionary *)args {
    MPMusicPlayerController *player = [MPMusicPlayerController systemMusicPlayer];
    if ([action isEqualToString:@"status"]) return MRStatusDict();
    if ([action isEqualToString:@"play"]) { [player play]; return MRStatusDict(); }
    if ([action isEqualToString:@"pause"]) { [player pause]; return MRStatusDict(); }
    if ([action isEqualToString:@"next"]) { [player skipToNextItem]; return MRStatusDict(); }
    if ([action isEqualToString:@"prev"]) { [player skipToPreviousItem]; return MRStatusDict(); }
    if ([action isEqualToString:@"toggle"]) {
        if (player.playbackState == MPMusicPlaybackStatePlaying) [player pause]; else [player play];
        return MRStatusDict();
    }
    if ([action isEqualToString:@"queueAlbum"]) return [self queueAlbum:args];
    if ([action isEqualToString:@"queueArtist"]) return [self queueArtist:args];
    if ([action isEqualToString:@"queueSong"]) return [self queueSong:args];
    if ([action isEqualToString:@"queueByID"]) return [self queueByID:args];
    if ([action isEqualToString:@"queueByStore"]) return [self queueByStore:args];
    if ([action isEqualToString:@"search"]) return [self searchSongs:args];
    if ([action isEqualToString:@"library"]) return [self libraryStats];
    if ([action isEqualToString:@"volume"]) return [self volumeAction:args];
    if ([action isEqualToString:@"like"]) return [self like:args];

    MRFail(@"unknown action \"%@\"", action);
    return @{};
}

- (NSDictionary *)like:(NSDictionary *)args {
    MPMediaItem *item = [MPMusicPlayerController systemMusicPlayer].nowPlayingItem;
    if (item == nil) MRFail(@"nothing is playing");
    NSNumber *pid = [item valueForProperty:MPMediaItemPropertyPersistentID];
    if (![pid isKindOfClass:[NSNumber class]]) MRFail(@"no track id for the playing song");
    BOOL on = YES;
    if ([args[@"on"] isKindOfClass:[NSNumber class]]) on = [args[@"on"] boolValue];
    MPMediaLibrary *library = [MPMediaLibrary defaultMediaLibrary];
    if (![library respondsToSelector:@selector(setValues:forProperties:forItemPersistentIDs:)]) {
        MRFail(@"this iOS version cannot set the liked state");
    }
    // 2 is loved, 0 is clear. Writing the database directly would race
    // medialibraryd, so go through the library object that owns the write.
    [library setValues:@[on ? @2 : @0]
          forProperties:@[@"likedState"]
  forItemPersistentIDs:@[pid]];
    NSMutableDictionary *data = [MRStatusDict() mutableCopy];
    data[@"liked"] = @(on);
    data[@"trackID"] = MRIDString(item, MPMediaItemPropertyPersistentID);
    return data;
}

- (NSDictionary *)queueAlbum:(NSDictionary *)args {
    NSString *query = MRQueryArgument(args);
    // albumsQuery comes back empty outside Music.app, so group songs by album
    // persistent ID and build the collections ourselves.
    NSMutableDictionary<NSNumber *, NSMutableArray<MPMediaItem *> *> *byAlbum = [NSMutableDictionary dictionary];
    for (MPMediaItem *song in [MPMediaQuery songsQuery].items ?: @[]) {
        id rawKey = [song valueForProperty:MPMediaItemPropertyAlbumPersistentID];
        NSNumber *key = [rawKey isKindOfClass:[NSNumber class]]
            ? rawKey
            : @([MRItemString(song, MPMediaItemPropertyAlbumTitle) hash]);
        NSMutableArray<MPMediaItem *> *bucket = byAlbum[key];
        if (bucket == nil) {
            bucket = [NSMutableArray array];
            byAlbum[key] = bucket;
        }
        [bucket addObject:song];
    }

    MPMediaItemCollection *exact = nil;
    NSMutableArray<MPMediaItemCollection *> *partial = [NSMutableArray array];
    NSMutableArray<NSString *> *seenTitles = [NSMutableArray array];

    for (NSArray<MPMediaItem *> *bucket in byAlbum.allValues) {
        NSString *title = MRItemString(bucket.firstObject, MPMediaItemPropertyAlbumTitle);
        if (title.length == 0) continue;
        [seenTitles addObject:title];
        MPMediaItemCollection *album = [MPMediaItemCollection collectionWithItems:MRSortedSongs(bucket)];
        if (MRIsExact(title, query)) {
            if (exact == nil) exact = album;
        } else if (MRContains(title, query)) {
            [partial addObject:album];
        }
    }

    MPMediaItemCollection *chosen = exact;
    NSMutableArray<NSString *> *others = [NSMutableArray array];
    if (chosen == nil && partial.count > 0) {
        // Pick the largest album, and report the other candidates.
        chosen = partial.firstObject;
        for (MPMediaItemCollection *album in partial) {
            if (album.count > chosen.count) chosen = album;
        }
        for (MPMediaItemCollection *album in partial) {
            if (album != chosen && others.count < 5) [others addObject:MRAlbumTitleForCollection(album)];
        }
    }
    if (chosen == nil) {
        NSMutableArray<NSString *> *quoted = [NSMutableArray array];
        for (NSString *title in MRNearbyAlbumTitles(seenTitles)) {
            [quoted addObject:[NSString stringWithFormat:@"\"%@\"", title]];
        }
        NSString *suffix = quoted.count > 0 ? [NSString stringWithFormat:@" (saw: %@)", [quoted componentsJoinedByString:@", "]] : @"";
        MRFail(@"no album matched \"%@\"%@", query, suffix);
    }

    NSString *matched = MRAlbumTitleForCollection(chosen);
    return [self finishQueue:MRSortedSongs(chosen.items) mode:MRModeArgument(args)
                     matched:matched others:others];
}

- (NSDictionary *)queueArtist:(NSDictionary *)args {
    NSString *query = MRQueryArgument(args);
    NSMutableArray<MPMediaItem *> *exact = [NSMutableArray array];
    NSMutableArray<MPMediaItem *> *partial = [NSMutableArray array];
    for (MPMediaItem *song in [MPMediaQuery songsQuery].items ?: @[]) {
        NSString *artist = MRItemString(song, MPMediaItemPropertyArtist);
        if (MRIsExact(artist, query)) [exact addObject:song];
        else if (MRContains(artist, query)) [partial addObject:song];
    }

    NSArray<MPMediaItem *> *chosen = exact.count > 0 ? exact : partial;
    if (chosen.count == 0) MRFail(@"no artist matched \"%@\"", query);
    NSArray<MPMediaItem *> *songs = MRSortedSongs(chosen);
    return [self finishQueue:songs mode:MRModeArgument(args)
                     matched:MRItemString(songs.firstObject, MPMediaItemPropertyArtist) others:@[]];
}

- (NSDictionary *)queueSong:(NSDictionary *)args {
    NSString *query = MRQueryArgument(args);
    NSString *artistFilter = MRStringArgument(args, @"artist");
    MPMediaItem *exact = nil;
    MPMediaItem *firstPartial = nil;

    for (MPMediaItem *song in [MPMediaQuery songsQuery].items ?: @[]) {
        if (artistFilter.length > 0 && !MRContains(MRItemString(song, MPMediaItemPropertyArtist), artistFilter)) {
            continue;
        }
        NSString *title = MRItemString(song, MPMediaItemPropertyTitle);
        if (MRIsExact(title, query)) {
            exact = song;
            break;
        }
        if (firstPartial == nil && MRContains(title, query)) firstPartial = song;
    }

    MPMediaItem *chosen = exact ?: firstPartial;
    if (chosen == nil) MRFail(@"no song matched \"%@\"", query);
    return [self finishQueue:@[chosen] mode:MRModeArgument(args) matched:MRItemString(chosen, MPMediaItemPropertyTitle) others:@[]];
}

// Library counts, for sanity checks after an OS or library change.
- (NSDictionary *)libraryStats {
    NSArray<MPMediaItem *> *songs = [MPMediaQuery songsQuery].items ?: @[];
    NSMutableSet<NSNumber *> *albums = [NSMutableSet set];
    NSMutableSet<NSString *> *artists = [NSMutableSet set];
    for (MPMediaItem *song in songs) {
        id albumID = [song valueForProperty:MPMediaItemPropertyAlbumPersistentID];
        if ([albumID isKindOfClass:[NSNumber class]]) [albums addObject:albumID];
        NSString *artist = MRItemString(song, MPMediaItemPropertyArtist);
        if (artist.length > 0) [artists addObject:MRNormalize(artist)];
    }
    MPMediaLibraryAuthorizationStatus auth = [MPMediaLibrary authorizationStatus];
    return @{@"songs": @(songs.count), @"albums": @(albums.count),
             @"artists": @(artists.count), @"auth": @(auth)};
}

- (NSDictionary *)searchSongs:(NSDictionary *)args {
    NSString *query = MRQueryArgument(args);
    NSInteger limit = 10;
    id rawLimit = args[@"limit"];
    if ([rawLimit respondsToSelector:@selector(integerValue)] && [rawLimit integerValue] > 0) {
        limit = MIN((NSInteger)[rawLimit integerValue], 50);
    }

    NSMutableArray<NSDictionary *> *items = [NSMutableArray array];
    for (MPMediaItem *song in [MPMediaQuery songsQuery].items ?: @[]) {
        NSString *title = MRItemString(song, MPMediaItemPropertyTitle);
        if (!MRContains(title, query)) continue;
        [items addObject:@{
            @"title": title,
            @"artist": MRItemString(song, MPMediaItemPropertyArtist),
            @"album": MRItemString(song, MPMediaItemPropertyAlbumTitle),
            @"duration": @(song.playbackDuration)
        }];
        if ((NSInteger)items.count >= limit) break;
    }
    return @{@"count": @(items.count), @"items": items};
}

- (NSDictionary *)volumeAction:(NSDictionary *)args {
    id rawValue = args[@"value"];
    if (![rawValue respondsToSelector:@selector(floatValue)]) {
        MRFail(@"volume needs a numeric \"value\"");
    }
    float value = MAX(0.0f, MIN(1.0f, [rawValue floatValue]));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [[MPMusicPlayerController systemMusicPlayer] setVolume:value];
#pragma clang diagnostic pop
    return @{@"volume": @(value)};
}

// Queue by Apple Music catalog (store) IDs, in order. This is the path that
// works for cloud items the process cannot enumerate, and it needs no library
// access at all. IDs arrive as strings because they are 64-bit.
- (NSDictionary *)queueByStore:(NSDictionary *)args {
    NSArray *rawIDs = args[@"ids"];
    if (![rawIDs isKindOfClass:[NSArray class]] || rawIDs.count == 0) {
        MRFail(@"queueByStore needs a non-empty \"ids\" array");
    }
    NSMutableArray<NSString *> *ids = [NSMutableArray arrayWithCapacity:rawIDs.count];
    for (id value in rawIDs) {
        NSString *text = [value isKindOfClass:[NSString class]] ? value : [value description];
        if (strtoull(text.UTF8String, NULL, 10) > 0) [ids addObject:text];
    }
    if (ids.count == 0) MRFail(@"no usable store ids");

    MPMusicPlayerStoreQueueDescriptor *descriptor =
        [[MPMusicPlayerStoreQueueDescriptor alloc] initWithStoreIDs:ids];
    NSString *start = MRStringArgument(args, @"start");
    if (start.length > 0) descriptor.startItemID = start;

    NSString *mode = MRModeArgument(args);
    MPMusicPlayerController *player = [MPMusicPlayerController systemMusicPlayer];
    // Queueing is always in album order, whatever shuffle was before.
    player.shuffleMode = MPMusicShuffleModeOff;
    if ([mode isEqualToString:@"replace"]) {
        [player setQueueWithDescriptor:descriptor];
        [player prepareToPlayWithCompletionHandler:^(NSError *error) {
            if (error != nil) NSLog(@"[MusicRemote] prepare failed: %@", error.localizedDescription);
        }];
        [player play];
    } else if ([mode isEqualToString:@"append"]) {
        [player appendQueueDescriptor:descriptor];
    } else if ([mode isEqualToString:@"next"]) {
        // Documented as "inserts after the now playing item": plays next.
        [player prependQueueDescriptor:descriptor];
    } else {
        MRFail(@"unknown mode \"%@\"", mode);
    }
    // Set again after the queue change: setting the queue re-applies the app's
    // own shuffle state, so the earlier assignment is not enough on its own.
    player.shuffleMode = MPMusicShuffleModeOff;

    NSMutableDictionary *data = [MRStatusDict() mutableCopy];
    data[@"mode"] = mode;
    data[@"queued"] = @(ids.count);
    return data;
}

// Queue by persistent ID. The player resolves the track list itself, so this
// sees the whole library even though this process cannot enumerate all of it.
- (NSDictionary *)queueByID:(NSDictionary *)args {
    NSString *property = MRStringArgument(args, @"property") ?: @"album";
    NSString *rawID = MRStringArgument(args, @"id");
    if (rawID.length == 0) MRFail(@"queueByID needs an \"id\" string");
    unsigned long long pid = strtoull(rawID.UTF8String, NULL, 10);

    NSString *key = MPMediaItemPropertyAlbumPersistentID;
    if ([property isEqualToString:@"artist"]) key = MPMediaItemPropertyArtistPersistentID;
    else if ([property isEqualToString:@"song"]) key = MPMediaItemPropertyPersistentID;
    else if (![property isEqualToString:@"album"]) MRFail(@"unknown property \"%@\"", property);

    MPMediaQuery *query = [MPMediaQuery songsQuery];
    [query addFilterPredicate:[MPMediaPropertyPredicate predicateWithValue:@(pid) forProperty:key]];
    NSString *mode = MRModeArgument(args);
    MPMusicPlayerController *player = [MPMusicPlayerController systemMusicPlayer];
    player.shuffleMode = MPMusicShuffleModeOff;

    if ([mode isEqualToString:@"replace"]) {
        MPMusicPlayerMediaItemQueueDescriptor *descriptor =
            [[MPMusicPlayerMediaItemQueueDescriptor alloc] initWithQuery:query];
        [player setQueueWithDescriptor:descriptor];
        [player prepareToPlayWithCompletionHandler:^(NSError *error) {
            if (error != nil) NSLog(@"[MusicRemote] prepare failed: %@", error.localizedDescription);
        }];
        [player play];
    } else if ([mode isEqualToString:@"append"]) {
        MPMusicPlayerMediaItemQueueDescriptor *descriptor =
            [[MPMusicPlayerMediaItemQueueDescriptor alloc] initWithQuery:query];
        [player appendQueueDescriptor:descriptor];
    } else {
        MRFail(@"unknown mode \"%@\"", mode);
    }

    NSMutableDictionary *data = [MRStatusDict() mutableCopy];
    data[@"mode"] = mode;
    data[@"property"] = property;
    data[@"id"] = rawID;
    data[@"visible"] = @([query items].count);
    return data;
}

// Shared tail of every queue action. It queues, then reports the live state
// so the device test can see whether Music.app actually started playing.
- (NSDictionary *)finishQueue:(NSArray<MPMediaItem *> *)songs mode:(NSString *)mode
                      matched:(NSString *)matched others:(NSArray<NSString *> *)others {
    if (songs.count == 0) MRFail(@"nothing to queue");
    MPMediaItemCollection *collection = [MPMediaItemCollection collectionWithItems:songs];
    MPMusicPlayerController *player = [MPMusicPlayerController systemMusicPlayer];
    player.shuffleMode = MPMusicShuffleModeOff;

    if ([mode isEqualToString:@"replace"]) {
        [player setQueueWithItemCollection:collection];
        [player prepareToPlayWithCompletionHandler:^(NSError *error) {
            if (error != nil) {
                NSLog(@"[MusicRemote] prepare failed: %@", error.localizedDescription);
            }
        }];
        [player play];
    } else if ([mode isEqualToString:@"append"]) {
        MPMusicPlayerMediaItemQueueDescriptor *descriptor =
            [[MPMusicPlayerMediaItemQueueDescriptor alloc] initWithItemCollection:collection];
        [player appendQueueDescriptor:descriptor];
    } else if ([mode isEqualToString:@"next"]) {
        MRFail(@"mode \"next\" is not supported in 0.1");
    } else {
        MRFail(@"unknown mode \"%@\"", mode);
    }

    NSMutableDictionary *data = [MRStatusDict() mutableCopy];
    data[@"queued"] = @(songs.count);
    data[@"mode"] = mode;
    if (matched.length > 0) data[@"matched"] = matched;
    if (others.count > 0) data[@"others"] = others;
    return data;
}

@end

%ctor {
    [[MRBridge sharedBridge] start];
}
