"""Writes fakes.inc: ISteamInventory/Apps/Friends/User/Utils subclasses for the fake libsteam_api,
generated from the Steamworks headers so every pure virtual is overridden.
Usage: gen_fakes.py <sdk>/public/steam <out file>"""
import re, sys
H = sys.argv[1].rstrip("/") + "/"
custom = {
 'ISteamInventory': {
  'GetResultStatus': 'return k_EResultOK;',
  'GetResultItems': 'return fake_GetResultItems(resultHandle, pOutItemsArray, punOutItemsArraySize);',
  'DestroyResult': 'fake_destroy(resultHandle);',
  'GetAllItems': 'return fake_GetAllItems(pResultHandle);',
  'ConsumeItem': 'return fake_ConsumeItem(pResultHandle, itemConsume, unQuantity);',
  'LoadItemDefinitions': 'return true;',
  'GetItemDefinitionProperty': 'return fake_DefProperty(iDefinition, pchPropertyName, pchValueBuffer, punValueBufferSizeOut);',
  'StartPurchase': 'return fake_StartPurchase(pArrayItemDefs, punArrayQuantity, unArrayLength);',
  'RequestPrices': 'return fake_RequestPrices();',
  'GetItemPrice': 'return fake_GetItemPrice(iDefinition, pCurrentPrice, pBasePrice);',
 },
 'ISteamApps': {
  'BIsDlcInstalled': 'return fake_dlcOwned(appID);',
  'GetDLCCount': 'return 1;',
  'BGetDLCDataByIndex': 'if (pAppID) *pAppID = 777; if (pbAvailable) *pbAvailable = true; snprintf(pchName, cchNameBufferSize, "Expansion"); return iDLC == 0;',
 },
 'ISteamFriends': {
  'ActivateGameOverlayToStore': 'fake_overlayStore(nAppID);',
 },
 'ISteamUser': { 'GetSteamID': 'return CSteamID(uint64(76561197960287930ULL));' },
 'ISteamUtils': { 'GetAppID': 'return 480;' },
}
files = {'ISteamInventory':'isteaminventory.h','ISteamApps':'isteamapps.h','ISteamFriends':'isteamfriends.h','ISteamUser':'isteamuser.h','ISteamUtils':'isteamutils.h'}
out = []
for cls, f in files.items():
    t = open(H+f, encoding='latin-1').read()
    m = re.search(r'class\s+'+cls+r'\s*\{(.*?)\n\};', t, re.S)
    body = m.group(1)
    body = re.sub(r'//[^\n]*', '', body)
    decls = re.findall(r'virtual\s+(.*?)\s*=\s*0\s*;', body, re.S)
    out.append(f'class Fake{cls} : public {cls} {{ public:')
    for d in decls:
        d = ' '.join(d.split())
        name = re.search(r'(\w+)\s*\(', d).group(1)
        ret = d[:d.index(name)].strip()
        # drop default args
        d2 = re.sub(r'\s*=\s*[^,)]+', '', d)
        if name in custom[cls]: b = custom[cls][name]
        elif ret == 'void': b = ''
        else: b = 'return {};'
        out.append(f'  {d2} override {{ {b} }}')
    out.append('};')
open(sys.argv[2],'w').write('\n'.join(out))
