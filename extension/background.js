// REICO Downloads: every download the browser starts is offered to REICO (through
// native messaging). If REICO takes it, the browser's own download is cancelled and
// REICO downloads it with several connections. If REICO isn't running, has the
// download manager turned off, or the download can't be handed over (blob:, data:,
// small files, private windows), the browser just downloads it as usual.

const HOST = 'com.reico.downloads';
const MIN_SIZE = 1024 * 1024; // smaller files gain nothing from extra connections

chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  const url = item.finalUrl || item.url;
  if (!/^https?:/i.test(url) || item.incognito || item.byExtensionId ||
      (item.totalBytes > 0 && item.totalBytes < MIN_SIZE)) {
    suggest();
    return;
  }
  chrome.cookies.getAll({ url }, cookies => {
    const msg = {
      url,
      name: (item.filename || '').split(/[\\/]/).pop(),
      size: item.totalBytes > 0 ? item.totalBytes : 0,
      referrer: item.referrer || '',
      cookies: (cookies || []).map(c => c.name + '=' + c.value).join('; '),
      ua: navigator.userAgent,
    };
    chrome.runtime.sendNativeMessage(HOST, msg, reply => {
      if (chrome.runtime.lastError || !reply || !reply.ok) { suggest(); return; }
      chrome.downloads.cancel(item.id, () => chrome.downloads.erase({ id: item.id }));
    });
  });
  return true; // we answer asynchronously
});
