// App Store (StoreKit) → the engine's "store" protocol (docs/plugins.md).
// The transaction observer is installed at init and stays for the app's lifetime, so purchases that
// complete later (Ask to Buy, interrupted payments, promoted purchases) reach the game as events.
#import <StoreKit/StoreKit.h>
#import "engine/platform/apple/RTSPlugin.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // original StoreKit API, see plugin.json

@interface RTSAppStorePlugin : NSObject <RTSPlugin, SKProductsRequestDelegate, SKPaymentTransactionObserver>
@end

@implementation RTSAppStorePlugin {
    NSMutableDictionary<NSString*, SKProduct*>* _products;
    NSMutableDictionary<NSValue*, RTSPluginReply*>* _requests;         // SKProductsRequest → reply
    NSMutableArray<SKProductsRequest*>* _running;                      // StoreKit holds requests weakly
    NSMutableDictionary<NSString*, RTSPluginReply*>* _purchases;       // productId → purchase() reply
    NSMutableDictionary<NSString*, SKPaymentTransaction*>* _unfinished; // token → transaction to finish
    NSMutableArray* _restored;
    RTSPluginReply* _restoreReply;
    BOOL _observing;
}

- (instancetype)init {
    if ((self = [super init])) {
        _products = [NSMutableDictionary new];
        _requests = [NSMutableDictionary new];
        _running = [NSMutableArray new];
        _purchases = [NSMutableDictionary new];
        _unfinished = [NSMutableDictionary new];
    }
    return self;
}

- (void)call:(NSString*)method args:(NSDictionary*)args reply:(RTSPluginReply*)reply {
    if ([method isEqualToString:@"init"]) {
        if (!_observing) { [SKPaymentQueue.defaultQueue addTransactionObserver:self]; _observing = YES; }
        [reply ok:@{@"canMakePayments": @(SKPaymentQueue.canMakePayments)}];
    } else if ([method isEqualToString:@"products"]) {
        NSMutableSet* ids = [NSMutableSet set];
        for (NSDictionary* p in args[@"products"]) if (p[@"id"]) [ids addObject:p[@"id"]];
        SKProductsRequest* req = [[SKProductsRequest alloc] initWithProductIdentifiers:ids];
        req.delegate = self;
        _requests[[NSValue valueWithNonretainedObject:req]] = reply;
        [_running addObject:req];
        [req start];
    } else if ([method isEqualToString:@"purchase"]) {
        NSString* pid = args[@"id"];
        SKProduct* product = _products[pid];
        if (!product) { [reply fail:[NSString stringWithFormat:@"unknown product %@: call products() first", pid]]; return; }
        if (!SKPaymentQueue.canMakePayments) { [reply fail:@"purchases are disabled on this device"]; return; }
        _purchases[pid] = reply;
        RTSPluginEmit(@"appstore", @"app.pause", nil);
        [SKPaymentQueue.defaultQueue addPayment:[SKPayment paymentWithProduct:product]];
    } else if ([method isEqualToString:@"restore"]) {
        _restoreReply = reply;
        _restored = [NSMutableArray array];
        for (SKPaymentTransaction* t in _unfinished.allValues) [_restored addObject:[self json:t state:@"restored"]];
        [SKPaymentQueue.defaultQueue restoreCompletedTransactions];
    } else if ([method isEqualToString:@"finish"]) {
        SKPaymentTransaction* t = _unfinished[args[@"token"] ?: @""];
        if (t) { [SKPaymentQueue.defaultQueue finishTransaction:t]; [_unfinished removeObjectForKey:args[@"token"]]; }
        [reply ok:nil];
    } else if ([method isEqualToString:@"lifecycle"]) {
        [reply ok:nil];
    } else {
        [reply fail:[@"appstore: unknown method " stringByAppendingString:method]];
    }
}

- (void)productsRequest:(SKProductsRequest*)request didReceiveResponse:(SKProductsResponse*)response {
    NSMutableArray* out = [NSMutableArray array];
    NSNumberFormatter* f = [NSNumberFormatter new];
    f.numberStyle = NSNumberFormatterCurrencyStyle;
    for (SKProduct* p in response.products) {
        _products[p.productIdentifier] = p;
        f.locale = p.priceLocale;
        NSDecimalNumber* micros = [p.price decimalNumberByMultiplyingByPowerOf10:6];
        NSMutableDictionary* j = [@{ @"id": p.productIdentifier, @"title": p.localizedTitle ?: @"",
                                     @"description": p.localizedDescription ?: @"",
                                     @"price": [f stringFromNumber:p.price] ?: @"",
                                     @"priceMicros": @(micros.longLongValue),
                                     @"currency": [p.priceLocale objectForKey:NSLocaleCurrencyCode] ?: @"" } mutableCopy];
        if (p.subscriptionPeriod) j[@"type"] = @"subscription";
        [out addObject:j];
    }
    NSValue* key = [NSValue valueWithNonretainedObject:request];
    [_requests[key] ok:out];
    [_requests removeObjectForKey:key];
    [_running removeObject:request];
}

- (void)request:(SKRequest*)request didFailWithError:(NSError*)error {
    NSValue* key = [NSValue valueWithNonretainedObject:request];
    [_requests[key] fail:error.localizedDescription ?: @"App Store request failed"];
    [_requests removeObjectForKey:key];
    if ([request isKindOfClass:SKProductsRequest.class]) [_running removeObject:(SKProductsRequest*)request];
}

- (NSDictionary*)json:(SKPaymentTransaction*)t state:(NSString*)state {
    NSString* token = t.transactionIdentifier ?: [NSUUID UUID].UUIDString;
    NSData* receipt = [NSData dataWithContentsOfURL:NSBundle.mainBundle.appStoreReceiptURL];
    return @{ @"productId": t.payment.productIdentifier ?: @"", @"state": state, @"token": token,
              @"orderId": t.originalTransaction.transactionIdentifier ?: token,
              @"receipt": @{ @"appStoreReceipt": [receipt base64EncodedStringWithOptions:0] ?: @"" } };
}

- (void)paymentQueue:(SKPaymentQueue*)queue updatedTransactions:(NSArray<SKPaymentTransaction*>*)transactions {
    for (SKPaymentTransaction* t in transactions) {
        NSString* pid = t.payment.productIdentifier;
        RTSPluginReply* r = _purchases[pid];
        switch (t.transactionState) {
        case SKPaymentTransactionStatePurchasing: break;
        case SKPaymentTransactionStateDeferred: // Ask to Buy: the parent decides later → event
            if (r) { [r ok:@{@"productId": pid, @"state": @"pending", @"token": @""}]; [_purchases removeObjectForKey:pid]; RTSPluginEmit(@"appstore", @"app.resume", nil); }
            break;
        case SKPaymentTransactionStatePurchased:
        case SKPaymentTransactionStateRestored: {
            BOOL restored = t.transactionState == SKPaymentTransactionStateRestored;
            NSDictionary* j = [self json:t state:restored ? @"restored" : @"purchased"];
            _unfinished[j[@"token"]] = t; // finished when the game calls finish() after granting
            if (restored && _restoreReply) [_restored addObject:j];
            else if (r) { [r ok:j]; [_purchases removeObjectForKey:pid]; RTSPluginEmit(@"appstore", @"app.resume", nil); }
            else RTSPluginEmit(@"appstore", @"purchase", j);
            break;
        }
        case SKPaymentTransactionStateFailed:
            [queue finishTransaction:t];
            if (r) {
                BOOL cancelled = t.error.code == SKErrorPaymentCancelled;
                [r fail:cancelled ? @"cancelled" : (t.error.localizedDescription ?: @"payment failed")];
                [_purchases removeObjectForKey:pid];
                RTSPluginEmit(@"appstore", @"app.resume", nil);
            }
            break;
        }
    }
}

- (void)paymentQueueRestoreCompletedTransactionsFinished:(SKPaymentQueue*)queue {
    [_restoreReply ok:_restored];
    _restoreReply = nil;
}

- (void)paymentQueue:(SKPaymentQueue*)queue restoreCompletedTransactionsFailedWithError:(NSError*)error {
    [_restoreReply fail:error.localizedDescription ?: @"restore failed"];
    _restoreReply = nil;
}

// Promoted in-app purchases started from the App Store page: let them run, the game gets an event.
- (BOOL)paymentQueue:(SKPaymentQueue*)queue shouldAddStorePayment:(SKPayment*)payment forProduct:(SKProduct*)product { return YES; }

@end

#pragma clang diagnostic pop
