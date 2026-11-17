# Teletext Server and Client

![Client showing news pages](docs/screenshots-news.png)

**Ready-built Windows programs:** download them from the
[Releases](https://github.com/bartleph/Teletext/releases) page, or build them yourself (below).

![Client showing news pages](docs/screenshots-news.png)

**Ready-built Windows programs:** download them from the
[Releases](https://github.com/bartleph/Teletext/releases) page, or build them yourself (below).

A Teletext service for your network. One server holds the pages and any number of
clients (on the same PC or across the LAN) connect to it and browse them like a 1980s TV set.

```
 +-----------------+         TCP port 8023          +------------------+
 |  ttxserver.exe  | <----------------------------> |  ttxclient.exe   |  (many)
 |  console app    |   GET 100  ->                  |  Win32 window    |
 |  pages\*.tti    |   <- PAGE frame (1000 bytes)   |  40 x 25 display |
 |  clock, carousel|   <- HDR every second          |                  |
 +-----------------+                                +------------------+
```

## Folder contents

| File | What it is |
|---|---|
| `server\ttxserver.cpp` | Server source (console application) |
| `server\pages\*.tti` | Demo pages 100-105 (the server recreates these if the folder is empty) |
| `client\ttxclient.cpp` | Client source (Windows GUI application) |
| `client\saa5050.h` | SAA5050 character ROM data, included by the client (keep it in the same folder) |
| `editor\ttxedit.cpp` | Page editor source (Windows GUI application) |
| `editor\saa5050.h` | Same ROM data, for the editor |
| `editor\ttxedit.cfg` | Editor settings: `pagesdir=` (the folder Open/Save start in), `server=`, `password=` |
| `client\ttxclient.cfg` | Client settings: host, port, start page, scaling |
| `server\ttxserver.cfg` | Server settings: port, service name, edit password, web port |
| `server\web\` | The browser/phone client served by ttxserver (index.html, sw.js, manifest, icons) |
| `feed\ttxfeed.cpp` | News feed program: turns RSS feeds (e.g. BBC News) into pages |
| `feed\ttxfeed.cfg` | Feed settings: which feeds go on which pages |
| `flights\ttxflights.cpp` | Airport arrivals/departures boards from a flight-data API |
| `vbi\ttxvbi.cpp` | Teletext packet stream for a real TV (Raspberry Pi composite out) |
| `tv\ttxtv.cpp` | TV listings pages from Freeview-EPG |
| `tv\ttxtv.cfg` | TV settings: channels, regions, pages |
| `weather\ttxweather.cpp` | Weather pages from Open-Meteo (free, no key) |
| `weather\ttxweather.cfg` | Weather settings: places and update interval |
| `flights\ttxflights.cfg` | Flights settings: API key, airports, daily request budget |
| `docs\` | Screenshots |

The **server** and **news feed** build on Windows *and* on Linux / Raspberry Pi, so they
can run on a headless machine. The **client** and **editor** are Windows programs that
connect to the server over the network.

## Building in Dev-C++

Each program is a single .cpp file, and each gets its own project.

**Server**
1. File > New > Project > **Console Application**, C++, name it `ttxserver`.
2. Remove the generated `main.cpp` and add `ttxserver.cpp` to the project.
3. Project > Project Options > Parameters > **Linker**: add `-lws2_32`
4. Compile & Run (F11). The first run creates the `pages` folder next to the exe.

**Client**
1. File > New > Project > **Windows Application**, C++, name it `ttxclient`.
2. Remove the generated `main.cpp` and add `ttxclient.cpp`.
3. Project > Project Options > Parameters > **Linker**: add `-lws2_32 -lgdi32`
4. Compile & Run. To test on one PC, start the server first.

**Editor**
1. File > New > Project > **Windows Application**, C++, name it `ttxedit`.
2. Remove the generated `main.cpp` and add `ttxedit.cpp`. Keep `saa5050.h` beside it.
3. Project > Project Options > Parameters > **Linker**: add `-lws2_32 -lgdi32 -lcomdlg32`
4. In `ttxedit.cfg`, set `server=` to your server's address (e.g. `192.168.1.20:8023`) and
   `password=` to its edit password. For local files, set `pagesdir=` too.

**News feed** (only needed if the server runs on Windows)
1. File > New > Project > **Console Application**, C++, name it `ttxfeed`.
2. Add `ttxfeed.cpp`. Linker: `-lwininet`

**Airport boards** (only needed if the server runs on Windows)
1. File > New > Project > **Console Application**, C++, name it `ttxflights`.
2. Add `ttxflights.cpp`. Linker: `-lwininet`

**No projects?** If you just open a .cpp and press Compile, project settings don't apply.
Instead, go to Tools > Compiler Options and tick *"Add the following commands when calling the
linker"*, then enter `-lws2_32 -lgdi32 -lcomdlg32 -lwininet`. Those settings cover all four programs.

All the programs are written in plain C++98, so they should build with the TDM-GCC 4.9 compiler in the original
Dev-C++ as well as the newer Embarcadero Dev-C++.

When you run from Dev-C++, the working folder is the project folder. That's where the
server makes `pages\` and where the client looks for `ttxclient.cfg`.

## Running across the network

* Start `ttxserver.exe`. It prints the addresses clients can use, e.g. `192.168.1.20:8023`.
* On each client PC, set `host=192.168.1.20` in `ttxclient.cfg`, or run
  `ttxclient 192.168.1.20 8023`.
* The first time it runs, Windows Firewall will ask whether to allow the server.
  Tick **Private networks**.
* Options: `ttxserver [port] [service-name]`, e.g. `ttxserver 8023 HOBBYTXT`.
  The service name (up to 8 characters) appears in every header row.

Server console keys: **R** reload pages, **L** list clients, **P** list pages, **Q** quit.
The server also checks the `pages` folder every 2 seconds. When a file is added, changed or
deleted, it reloads and pushes the new version to anyone viewing that page. So you can
save from the editor and watch a client update a moment later.

## Running the server on a headless Linux machine or Raspberry Pi

The server and news feed only need `g++` and `curl`, so they run happily on a Pi.

**1. Copy the files across.** Copy the `server` and `feed` folders to the Pi, for example
into `~/teletext/` (WinSCP works well for this from Windows).

**2. Build** (on the Pi):
```
sudo apt install g++ curl
cd ~/teletext/server && g++ -O2 -o ttxserver ttxserver.cpp
cd ~/teletext/feed   && g++ -O2 -o ttxfeed   ttxfeed.cpp
cd ~/teletext/flights && g++ -O2 -o ttxflights ttxflights.cpp
cd ~/teletext/weather && g++ -O2 -o ttxweather ttxweather.cpp
cd ~/teletext/tv      && g++ -O2 -o ttxtv ttxtv.cpp
```

**3. Settings.** In `~/teletext/server/ttxserver.cfg`:
```
port=8023
name=TELETEXT
password=choose-something
```
In `~/teletext/feed/ttxfeed.cfg`, change the first line to `pagesdir=../server/pages`
(forward slashes on Linux).

**4. Try it.** Run `./ttxserver` in the server folder. Type `p` then Enter to list pages,
and `q` then Enter to stop. Point a Windows client at the Pi (`host=` the Pi's address in
`ttxclient.cfg`). `hostname -I` on the Pi shows its address.

**5. Start automatically at boot.** Create `/etc/systemd/system/ttxserver.service`
(change `pi` to your user name and folder):
```
[Unit]
Description=Teletext server
After=network-online.target
Wants=network-online.target

[Service]
User=pi
WorkingDirectory=/home/pi/teletext/server
ExecStart=/home/pi/teletext/server/ttxserver
Restart=always

[Install]
WantedBy=multi-user.target
```
Then make `/etc/systemd/system/ttxfeed.service` the same way, with `Description=Teletext
news feed`, `WorkingDirectory=/home/pi/teletext/feed` and
`ExecStart=/home/pi/teletext/feed/ttxfeed`. Enable both:
```
sudo systemctl daemon-reload
sudo systemctl enable --now ttxserver ttxfeed
journalctl -u ttxserver -f        (watch the log; Ctrl+C to stop watching)
```
If the Pi runs a firewall (`ufw`), open the port: `sudo ufw allow 8023/tcp`.

The server is plain TCP, so keep it on your home network. Don't publish it through your
Cloudflare tunnel without extra protection, because the edit password is sent unencrypted.

## Editing pages on the server (ttxedit)

With `server=` and `password=` set in `ttxedit.cfg`:

* **File > Open from server** (Ctrl+Shift+O). The status bar lists the server's pages.
  Type a page number and press Enter.
* Edit as usual, then press **Ctrl+S**. A page that came from the server is saved back
  to the server, and anyone viewing it sees the change at once.
* **File > Save to server** (Ctrl+Shift+S) sends any page to the server, including a new
  page or one opened from a local file. It is stored as `Pnnn.tti` under its page
  number, so changing the number with Ctrl+P and saving creates a new page.
* **File > Delete page from server** removes a page's `Pnnn.tti` file.

The server only accepts saves and deletes with the right password. With no password set,
anyone on your network can change pages, and the server warns about this when it starts.
Viewing never needs a password.

## Editing from a tablet or phone (web editor)

![Web editor](docs/screenshots-webedit.png)

The server also has a touch-friendly editor in the browser, so you can edit pages from an
Android tablet or a phone. It uses the same port as the browser viewer:

    http://<server address>:8080/edit.html

To open a particular page straight away, add `?p=` and the page number, e.g. `edit.html?p=200`.

* **Open / New / Save** are at the top. *Page settings* sets the page number, the seconds
  per subpage, the coloured-key links and a description. The title shows `unsaved` until
  the page is saved.
* **Typing:** tap a cell on the screen to put the cursor there, then tap the *Type* box and
  type on the tablet's keyboard. The arrow, Enter, Backspace, Ins and Del buttons work as
  on a PC keyboard. `#` and `£` come out as Teletext # and £.
* **Colours and effects:** the buttons put a control code at the cursor: text colours,
  graphics colours, double height, new/black background, flash, conceal, separated
  graphics, hold graphics and so on. *Show codes* shows the codes as grey letter cells,
  as in ttxedit.
* **Drawing:** put a graphics colour code at the start of a row, choose *Draw*, and draw
  with your finger in the cells after it. *Erase* rubs out. Each finger stroke is one Undo.
* **Rows and subpages:** insert, delete or clear rows; add, copy and remove subpages.
* **Delete page from server** removes the page's file.

Saving needs the server's `password=` from `ttxserver.cfg`. The editor asks for it the
first time; tick *Remember on this device* to save it in the browser.
* With no password set, the web editor cannot save at all.
* After 5 wrong passwords, the server refuses any password for a minute.
* Editing through a Cloudflare tunnel is refused unless you set `webeditremote=1` in
  `ttxserver.cfg`, so the public viewer address stays view-only. If you turn it on, put
  Cloudflare Access in front of the site, as the password travels with each save.

## News from the internet (ttxfeed)

`ttxfeed` downloads RSS news feeds every 10 minutes and writes them as pages. Each section
gets a headlines page with up to 9 stories, plus one page per story (headline, summary and
time):

| Pages | Section (default `ttxfeed.cfg`) |
|---|---|
| 110, stories 111-119 | BBC News top stories |
| 120 / 130 / 140 / 150 | UK, World, Business, Technology |
| 300, 360 | BBC Sport, Formula 1 |

* The coloured keys work like Ceefax. On a headlines page: red = top story,
  green/yellow = next/previous section, cyan = index (`indexpage=`, default 100). On a
  story: red = next story, green = back to the headlines.
* Add or change sections with `feed=page,TITLE,url` lines. Use a page ending in 0, because
  the next 9 numbers are used for the stories. Most BBC News and Sport pages have a feed at
  the same address on `feeds.bbci.co.uk` with `/rss.xml` on the end, e.g.
  `https://feeds.bbci.co.uk/sport/football/rss.xml`.
* **Breaking news, page 190:** the newest story from all the feeds. If it was published in
  the last hour (`breakingminutes=60`), the page shows a flashing red BREAKING NEWS banner
  and the headline in double height. Otherwise it says there's no breaking news and shows
  the latest headline. Red goes to the full story, green to that section's headlines.
  Set `breakingpage=0` to turn it off.
* **Newsflash on every screen:** while a story counts as breaking, every client shows a
  strip across the bottom row, whatever page it is on. "NEWSFLASH" flashes on red, the
  headline scrolls past on blue, and the PC beeps once when it arrives. Click the strip or
  press **F6** to read the story; press **Esc** to dismiss it. The strip goes away by
  itself when the story is more than `breakingminutes=` old. (ttxfeed passes the headline
  to the server in `pages/newsflash.txt`.)
* A page is only rewritten when the news changes. If a download fails, the old pages stay.
* Run it on the same machine as the server, with `pagesdir=` pointing at the server's
  `pages` folder. On Linux it downloads with `curl`; on Windows with WinINet.
* Don't edit pages 110-119 and so on by hand: the next update overwrites them. Add links to
  the news sections on your index page (100) with the editor instead.
* The feeds carry headlines and a one-line summary only. The full stories are on the BBC
  website. The pages say "Source: BBC News". This is fine for personal use at home, but
  don't republish the pages publicly.

## On a real TV (ttxvbi)

`ttxvbi` sends your pages to a real TV's own teletext decoder (the Text button on the
remote). It reads the server's pages folder and writes a stream of teletext packets to
standard output. On a Raspberry Pi you pipe that into
[raspi-teletext](https://github.com/ali1234/raspi-teletext), which puts the packets into the
vertical blanking interval of the Pi's **composite video** output:

```
g++ -O2 -o ttxvbi ttxvbi.cpp
./ttxvbi -d ../server/pages | sudo teletext -
```

Connect the Pi's composite output to the TV (a composite-to-SCART lead works) and press
Text. Needs a TV that decodes analogue teletext: mostly CRTs and older flat screens. Many
current TVs have dropped it, and Freeview/DVB teletext is a different thing (see below).
Check raspi-teletext's own README for which Pi models and video settings it supports.

* Every page and subpage in the folder is sent, so everything the server, editor, `ttxfeed`,
  `ttxweather` and the others write is on air. Pages are reloaded when the files change.
* The header shows the service name (`name=` from `ttxserver.cfg`, or `-n NAME`), the page
  number, the date and a live clock. Carousel pages rotate every `CT` seconds.
* Fastext links are sent, so the TV's red/green/yellow/cyan buttons work. Cyan is the
  fourth link, as in the web client; the TV's own index key uses the sixth.
* While there is breaking news (`pages/newsflash.txt`, written by `ttxfeed`), the bottom
  row of every page becomes the NEWSFLASH strip with the headline scrolling past. This
  replaces that page's Fastext labels while the flash is on.
* Options: `-d pagesdir`, `-n NAME`, `-r packets/sec` to limit the speed (normally the
  pipe sets the pace), `-c count` to stop after that many packets, and `-p page` to
  dump a page as text (below).
* **Checking a page without a TV:** `./ttxvbi -d ../server/pages -p 100` prints page 100
  as it would go on air: the packets are built exactly as for the TV, then decoded again
  and drawn as a 40 x 25 grid. It shows the header, rows that are blank and so not sent,
  the Fastext links, the newsflash row if one is on, and whether every Hamming and parity
  check passed. A carousel page prints each subpage in turn. Exit code 0 means OK, 1 means
  no such page, 2 means the decode found errors. Colour and graphics control codes show
  as blank cells, as they do on a real teletext screen.
* Teletext on air is a one-way broadcast: the TV picks pages out of the cycle, so a page
  takes a moment to appear after you key it in, and more pages make the cycle longer.
* Linux only. It does not touch DVB (Freeview) teletext, which needs a DVB modulator.

## TV listings (ttxtv)

![TV listings](docs/screenshots-tv.png)

`ttxtv` builds Ceefax-style TV listings from **Freeview-EPG**, an open-source project
that publishes free listings for every UK Freeview channel (for personal use) as an
XMLTV file on GitHub. No key or sign-up is needed.

| Page | Contents |
|---|---|
| 600 | TV Now and Next: every channel's current programme, and what's next with its start time |
| 601-609 | One page per channel: listings from now, with the programme on now in yellow and a heading at each new day. They rotate through up to 3 subpages, into tomorrow |

* Coloured keys: red/green = next/previous channel, yellow = Now and Next, cyan = index.
* It downloads the listings (about 22 MB) every 4 hours (`download=240`). The source is
  regenerated every 8 hours. It rebuilds the pages from the saved copy (`epg.xml`, kept
  next to the program) every 5 minutes, so "now" moves along. After a restart it reuses
  a recent saved copy instead of downloading again.
* **Channels and regions** are `channel=id,NAME,page` lines in `ttxtv.cfg`. The defaults
  use the South region (BBC One South, ITV1 Meridian South, Channel 4 South). For your own
  region, swap in e.g. `BBCOneLondonHD.uk` / `ITV1London.uk`, `BBCOneWest.uk` /
  `ITV1West.uk`, `BBCOneNorthWest.uk` / `ITV1Granada.uk`, `BBCOneScotHD.uk`,
  `BBCOneWalesHD.uk` / `ITV1Wales.uk`. The console warns about any id with no programmes.
  The full list of ids is the `<channel id="...">` lines at the top of `epg.xml`.
* Run it on the server machine, like the other feeds, and make a `ttxtv` service the same
  way.

## Weather (ttxweather)

`ttxweather` builds Ceefax-style weather pages from **Open-Meteo**, which is free for
non-commercial use and needs no key or sign-up:

| Page | Contents |
|---|---|
| 400 | Weather index: every place with its temperature and conditions |
| 401-409 | One page per place |

Each place's page has:
* current temperature, conditions, wind and humidity
* a mosaic weather symbol (sun, sun and cloud, cloud, fog, rain, snow, thunder)
* a 5-day forecast with highs, lows and chance of rain
* today's sunrise and sunset times

Places are set in `ttxweather.cfg` as `place=NAME,latitude,longitude,page`. To find a
place's latitude and longitude, right-click it in Google Maps. It updates every 30
minutes (`interval=`), one request per place, well inside Open-Meteo's fair-use limit.
Every page carries the "Weather data by Open-Meteo.com" credit that their licence
(CC BY 4.0) asks for.

Run it like the other feeds: on the server machine, with `pagesdir=` pointing at the
server's `pages` folder. On the Pi, copy the `weather` folder next to `server`, build it,
and make a `ttxweather` service the same way as for the others.

## Airport arrivals and departures (ttxflights)

`ttxflights` fetches live flight lists from a flight-data API and builds Ceefax-style
boards:

| Page | Board |
|---|---|
| 410 | Airports index |
| 411 / 412 | Southampton arrivals / departures |
| 413 / 414 | Bristol |
| 415 / 416 | Heathrow |
| 417 / 418 | Gatwick |

* Each line shows the time, flight, from/to and status: On time, Exp 14:35, Boarding,
  Gate shut, Lnd 14:31, Dep 14:40, In flight, Cancelled (in red), and so on. Codeshare
  duplicates are left out.
* Busy airports get up to 3 subpages that rotate every 10 seconds.
* Coloured keys: red = the other board for that airport, green/yellow = next/previous
  airport, cyan = index.
* Run it on the same machine as the server, with `pagesdir=` pointing at the server's
  `pages` folder (as for ttxfeed).

### Using FlightAware AeroAPI (`ttxflights.cfg`, the default)

1. Sign up for a **Personal** AeroAPI account on flightaware.com and create an API key.
   The Personal tier includes $5 of free queries a month, or $10 for ADS-B feeders.
   Personal use only.
2. Put the key in `ttxflights.cfg` as `apikey=`.
3. In the AeroAPI portal, look up the price per result set for
   `/airports/{id}/flights/scheduled_arrivals` and `scheduled_departures`, and set
   `pageprice=` to it. Set `monthlybudget=` a little under your free allowance.
4. Airports use 4-letter ICAO codes: EGHI Southampton, EGGD Bristol, EGLL Heathrow,
   EGKK Gatwick.

ttxflights keeps a running total of what it has spent this month in `ttxflights.state`,
which survives restarts. It never starts an update that could take it over
`monthlybudget=`, and it spaces updates through `hours=` to spread the money across the
month. The console shows the cost per update and the resulting update interval when it
starts. To update more often, use fewer airports or a shorter day. Requests are spaced
`gap=7` seconds apart to stay inside the Personal tier's limit of 10 result sets a minute.
`maxpages=1` gives up to 15 flights per board; 2 gives 30 but costs twice as much.

### Using MyAirports (`ttxflights-myairports.cfg`)

Copy it over `ttxflights.cfg` to use MyAirports instead. It uses a free key from
myairports.online, IATA codes (SOU, BRS, LHR, LGW) and a budget of requests per day
(`budget=95` for the free 100 a day). At the time of writing, MyAirports' Cloudflare
protection was blocking API requests that come from programs, so it may not work.

### Other APIs and troubleshooting

* Each raw reply is saved as `last_<code>_<type>.json`. If a board stays empty, or the
  console says "none usable", that file shows what the API sent.
* `url=` takes `{code}`, `{type}` (arrivals/departures), `{pages}` and `{key}`.
  `header=` lines set the HTTP headers (`{key}` is replaced with your key). The program
  reads most JSON flight lists and finds the usual field names by itself.

## Phones, tablets and other browsers

`ttxserver` has a built-in web client. On any phone, tablet or PC on your network, open
a browser at

    http://<server address>:8080          e.g. http://192.168.1.20:8080

![Browser client on a phone](docs/screenshots-phone.png)

It draws pages with the same SAA5050 lettering as the Windows client, including mosaics,
flashing text, subpages and the newsflash strip, and it updates once a second.

* **Controls:** the four coloured Fastext buttons, a number pad, previous/next page,
  Index, Reveal, Hold, and previous/next subpage. **Swipe** the screen left or right to
  change page, or tap a page number or the newsflash strip. On a PC the keyboard works as
  in the Windows client.
* **Smooth** switches between soft, TV-like scaling and sharp pixels.
* Turn a phone on its side for a bigger picture, with the buttons alongside.
* **Make it an app:** see *Installing it as an app (PWA)* below. Even over plain http,
  Chrome's menu › **Add to Home screen** gives a home-screen shortcut.
* When a newsflash arrives, the phone vibrates (Android) and a *Dismiss newsflash* button
  appears.

The page itself is `server/web/index.html` (plus `manifest.json` and two icons), so copy
the `web` folder along with the server. `webport=` in `ttxserver.cfg` changes the port;
`webport=0` turns it off. Open the port in the firewall if you use one
(`sudo ufw allow 8080/tcp` on the Pi; Windows asks the first time). The web client only
views pages. Editing still needs `ttxedit` and the password.

### Installing it as an app (PWA)

The web client is a Progressive Web App. It has a manifest, icons including an Android
"maskable" one, and a service worker (`web/sw.js`) that keeps a copy of the app so it
opens instantly, even when the server can't be reached. Page data is always live. When
Chrome offers to install it, an **Install** button appears among the controls.

Browsers only allow this over **HTTPS**, so it needs a proper web address rather than
`http://192.168.x.x:8080`. The easy way is a Cloudflare tunnel, if you run one on the
same machine:

1. **Add a public hostname to the tunnel**, e.g. `teletext.yourdomain.co.uk` →
   `http://localhost:8080`.
   * Tunnel managed in the Cloudflare dashboard: Zero Trust › Networks › Tunnels › your
     tunnel › Public Hostname › Add. Enter subdomain `teletext`, your domain, and
     service type `HTTP` with URL `localhost:8080`.
   * Tunnel managed by a `config.yml` file: add these lines above the final catch-all
     rule, then run `cloudflared tunnel route dns <tunnel-name> teletext.yourdomain.co.uk`
     and `sudo systemctl restart cloudflared`:
     ```
       - hostname: teletext.yourdomain.co.uk
         service: http://localhost:8080
     ```
2. **Restrict it to yourself (recommended):** Zero Trust › Access › Applications › Add
   an application › Self-hosted, for `teletext.yourdomain.co.uk`, with a policy that
   allows only your own email address. You log in once on each phone. This keeps your
   pages private and keeps licensed data (FlightAware) to personal use.
3. On the phone, open `https://teletext.yourdomain.co.uk` in Chrome, then tap **Install**
   on the page, or ⋮ › **Install app**.

Only the read-only web port goes through the tunnel. Editing (port 8023) stays on your
home network.

## Client keys

| Key | Action |
|---|---|
| 0-9 | Type a three-digit page number (100-899) |
| Up / Down (PgUp / PgDn) | Next / previous page |
| Left / Right | Step through subpages (this stops the carousel for that page) |
| F1 F2 F3 F4 | Fastext red, green, yellow, cyan |
| F5 or I | Index page |
| R | Reveal or hide concealed text |
| H | Hold: freeze the page so the carousel doesn't change it |
| Esc | Cancel a half-typed page number, or dismiss a newsflash |
| F6 | Go to the newsflash story |
| Mouse | Click any 3-digit page number on screen, or a coloured Fastext link |

You can resize or maximise the window. The page always keeps a 4:3 TV shape.
`scaling=smooth` (the default) gives a soft CRT-like edge, and `scaling=sharp` gives
hard-edged pixels.

## Page editor (ttxedit)

The editor shows the page exactly as the client will, using the same SAA5050 font, and
edits it cell by cell, like a real teletext page. Every control code takes up one
character cell. With **View > Show control codes** on (Tab), each code shows as its Esc
letter on a grey square. Text-colour codes show the letter in their colour; graphics-colour
codes show it on a block of their colour. The status bar names the code under the cursor,
e.g. `Code 1D New background (Esc ])`. Codes whose Esc character is `\ ] ^ _` show as
½ → ↑ #, because that is how the UK character set draws those characters.

| Key | Action |
|---|---|
| Typing | Writes at the cursor, then moves right |
| Arrows, Home, End, Enter | Move the cursor |
| Backspace / Del / Ins | Erase the previous cell / delete a cell / insert a space |
| Ctrl+Ins / Ctrl+Del | Insert / delete a whole row |
| F1-F7 | Text colour: red, green, yellow, blue, magenta, cyan, white |
| Shift+F1-F7 | Graphics colour (same order) |
| F8 / Shift+F8 | Flash / steady |
| F9 / Shift+F9 | Double height / normal height |
| F11 / Shift+F11 | New background / black background |
| F12 / Shift+F12 | Separated / contiguous graphics |
| Esc, then a letter | Any code, using the same letters as .tti files (Esc A = red, Esc X = conceal, Esc ^ = hold graphics...) |
| Ctrl+G | **Draw mode**: left mouse button draws mosaic blocks, right button erases. Works in cells to the right of a graphics colour code. In text mode, clicking moves the cursor |
| Tab / Ctrl+R | Show control codes / reveal concealed text |
| PgUp / PgDn | Previous / next subpage (the **Subpage** menu adds, duplicates and deletes them) |
| Ctrl+P | Page settings in the status bar: page number, carousel time, Fastext links, description. Enter moves to the next setting, Esc finishes |
| Ctrl+Z / Ctrl+Y | Undo / redo (up to 200 steps) |
| Ctrl+N / Ctrl+O / Ctrl+S | New / open / save |
| Ctrl+H | Key summary |

The **Insert** menu lists all 31 control codes, in case you forget a key.
`ttxedit P200.tti` opens a file straight away. Ctrl+N starts a blank page and asks for
its number. The first save suggests the name `P200.tti` in your `pagesdir`.

**Typical workflow:** start the server, open a client on the page, edit in ttxedit and
press Ctrl+S. The client shows the change within a couple of seconds.

**Making a banner:** on a row, press F4, F11, then F3 (blue, new background, yellow text),
then type. For a big title, press F9 first to make it double height. Leave the row
below empty, because it is used for the lower half of the tall letters.

**Drawing:** put a graphics colour code at the start of a row (e.g. Shift+F7, white),
press Ctrl+G, then draw. Each character cell is 2×3 blocks. For a coloured background
behind your drawing, use Shift+F4, F11, Shift+F3 (blue, new background, yellow graphics).

## Design

### Server
* **One thread with `select()`.** A single loop waits on the listening socket and all client
  sockets, waking at least 5 times a second. There are no threads, so there are no locking
  problems. The limit is 200 clients, which is plenty for a LAN. (Raise `MAX_CLIENTS` and
  `FD_SETSIZE` together if you need more.)
* **Page store:** `std::map<int, Page>`, keyed by page number (hex, 0x100-0x8FF, as on a real
  service). Each `Page` has a vector of `SubPage`s (25 x 40 bytes), a cycle time, and
  Fastext links.
* **Carousels:** once a second the server moves on any multi-subpage page whose cycle time
  is up, and pushes the new subpage to every client viewing it. Clients that asked for a
  specific subpage are left alone.
* **Header row:** row 0 is never stored. The server builds it live, with the service name,
  page number, date and a yellow clock, and sends it to each client every second.
* **Pages** are loaded from `.tti` files at start-up and when you press R.

### Client
* **Win32 + `WSAAsyncSelect`.** Windows posts a `WM_SOCKET` message to the window when the
  socket connects, has data or closes, so the whole client runs in the normal message loop
  with no threads. If the connection fails or drops, it retries every 5 seconds.
* **Rendering:** no Windows fonts are used. `saa5050.h` holds the character ROM of the
  Mullard SAA5050 teletext chip (96 characters, 5x9 dots in a 6x10 cell). At start-up each
  character is doubled to 12x20 pixels and given the chip's *character rounding*: a
  half-dot is added wherever two dots touch only diagonally. That is what makes real
  teletext text look smooth and chunky. The page is drawn pixel by pixel into a 480x500
  buffer, the SAA5050's own resolution, then stretched to a 4:3 picture with
  `StretchDIBits`. A timer toggles flashing text twice a second.
* **Attribute decoding** follows Level 1 Teletext. "Set-at" codes (steady, normal size,
  conceal, contiguous/separated, black/new background, hold) take effect in their own cell.
  "Set-after" codes (colours, flash, double height, release) take effect from the next cell.
  Mosaic characters are drawn as 2x3 blocks, so they look right with any font.
* The UK character set is used: `#` shows as £, `[` as ←, `\` as ½, and so on.
* Font data: the SAA5050 (UK) bitmap from the *hoard-of-bitfonts* collection
  (github.com/robhagemans/hoard-of-bitfonts), released into the public domain.

### Protocol (TCP, port 8023)

Client to server: one command per line, ending in `\n`.

| Command | Meaning |
|---|---|
| `GET ppp` | Show page ppp (hex digits, e.g. `GET 100`) and follow its carousel |
| `GET ppp s` | Show subpage s (1-based) of page ppp, without rotating |
| `LIST` | Reply `PAGES ppp ppp ...` |
| `GETTTI ppp` | Reply `TTI ppp len` + len bytes: the page as .tti text (for the editor) |
| `AUTH password` | Reply `OK` or `ERR ...`; needed before saving or deleting when a password is set |
| `PUTTTI ppp len` + len bytes | Save the .tti text as `pages/Pppp.tti`. Reply `OK ppp` or `ERR ...` |
| `DELETE ppp` | Delete `pages/Pppp.tti`. Reply `OK ppp` or `ERR ...` |
| `QUIT` | Disconnect |

The web client uses plain HTTP on the web port: `GET /` serves `web/index.html`, and
`GET /api/page?p=100&s=0` returns the page as JSON. The JSON holds 25 rows of 40 bytes
as hex, the subpage, links and any newsflash. `s` is a fixed subpage, or 0 to follow
the carousel.

Server to client: a text line, then fixed-size binary data.

| Message | Followed by |
|---|---|
| `PAGE ppp s n red green yellow cyan index\n` | 1000 bytes: 25 rows x 40 columns, 7-bit Teletext codes. `s`/`n` = subpage / count. Links are hex page numbers, `000` = none |
| `HDR\n` | 40 bytes: a new header row (clock update) |
| `NOPAGE ppp\n` | nothing |
| `FLASH ppp headline\n` | nothing: show a newsflash for story page ppp (`FLASH\n` alone clears it) |

Because the frame sizes are fixed, the client can always tell where one message ends. It
waits until a whole frame has arrived before using it. You could write a client in
anything, even a 6502 with a serial-to-TCP adapter.

## Making your own pages (.tti format)

A `.tti` file is plain text, one command per line:

```
DE,My weather page
CT,10,T               <- seconds per subpage (if more than one)
PN,20000              <- page 200, subpage 00
OL,1,<ESC>M<ESC>CWEATHER
OL,4,<ESC>FSunny spells
PN,20001              <- another subpage of 200: makes a carousel
OL,1,...
FL,100,201,202,203,8FF,100   <- Fastext: red,green,yellow,cyan,(unused),index
```

* `OL,n,` sets row n (1-24). Row 0 is ignored because the server makes its own header.
* Control codes are written as the ESC byte (0x1B) followed by the code + 0x40. For
  example, ESC `A` = alpha red (0x01), ESC `M` = double height (0x0D), ESC `]` = new
  background (0x1D). Codes stored with bit 7 set (0x80-0x9F) are also accepted.
* Several pages can be in one file, or one per file. Drop the files in `pages\` and press
  **R** on the server.
* Teletext editors that save `.tti` files will work. You can also copy and edit the demo
  files.

### Control code reference
| Code | ESC+ | Meaning | | Code | ESC+ | Meaning |
|---|---|---|---|---|---|---|
| 01-07 | A-G | Text red..white | | 11-17 | Q-W | Graphics red..white |
| 08 | H | Flash | | 18 | X | Conceal |
| 09 | I | Steady | | 19 | Y | Contiguous graphics |
| 0C | L | Normal height | | 1A | Z | Separated graphics |
| 0D | M | Double height | | 1C | \ | Black background |
| | | | | 1D | ] | New background |
| | | | | 1E | ^ | Hold graphics |
| | | | | 1F | _ | Release graphics |

Colours: 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white.

## Ideas for extending it
* **Live pages:** build a page in code every few seconds (server stats, news from a file
  your scripts update) the same way `MakeHeader` builds row 0.
* **Rolling header:** while a client waits for a page, cycle the header page number like a
  real set searching the carousel.
* **Level 2.5:** extra colours and double width (codes 0x0E, 0x0F) are currently ignored.
