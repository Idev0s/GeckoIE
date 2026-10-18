# GeckoIE

Hi! So GeckoIE is a project aiming to port Firefox's Gecko engine into Microsoft Internet Explorer 6(+ down the line).


## How It Works

Right now, the project is in a super early stage that basically just docks a tollbar-less Mypal (or whatever firefox related) window into an IE window. The goal is to get a full, raw Engine port, that will rended natively inside IE Shell. 

## Status
 
The project is split into four stages. It is currently in stage 2.
 
| Stage | State | What it means |
|---|---|---|
| 1. Window hosted prototype | Done | A Mypal (74.1.4) engine runs as a hidden helper program and its window is docked over IE's page area. |
| 2. Synced browser shell | In progress | IE's address bar, title, Back/Fwd, Refresh and history sync with the engine. Back/Fwd, multiple IE windows are still being tested. IE's Stop button is not connected yet. |
| 3. Raw engine port | Research | The goal is to run Gecko's engine inside IE itself. Running it inside IE's own process has not been tried yet. |
| Final. Optimizations | Planned | Severely decrease disk usage, lowering RAM usage and background processes. |
 
> Used Mypal 74.1.4. GeckoIE does not change Mypal's program files.

## Compatibility Info

- Target: **Windows XPx86 running Internet Explorer 6.**
- Tests: Windows XPx86 Internet Explorer 6..8: Works, though later versions than 6, may have bugs and it's currently not supported.
Windows Vista and up with IE7+: Does **NOT** work at all right now.
> Windows 11 with Edge definitely works(uhum).

|                |Internet Explorer v.                         |Windows v.                         |
|----------------|-------------------------------|-----------------------------|
|Works|6, 7, 8            |XP            |
|Does not work          |9+            |Vista+            |

## Requirements
 
- Windows XP, 32-bit, with Internet Explorer 6.
- **.NET Framework 2.0 or newer.** The installer builds the IE add-on on your PC with the .NET compiler.
- A copy of Firefox that works on XP. GeckoIE does not include Mypal. Mypal's source: <https://github.com/Feodor2/Mypal68>.

## How to install

- Download a version of Firefox based browser that works on XP, recommend [Mypal](https://www.mypal-browser.org/download.html).
- If your Mypal's program has another name, rename it to firefox.exe first.
- Download the setup file from Releases. 
- Run setup, it will ask you for:
  - Where to install the project
  - Where you have your browser folder with firefox.exe
  - Slim the engine, selecting it, unused files will be moved to "engine-removed" and can be restored using 'restore-engine.bat'
- Select if you want to slim the setup.
- Run Internet Explorer 6 and expect bugs.
> - You can still run legacy IE6 engine either through the Start menu shortcut or just by disabling the addon.

## Using it
 
- Every IE window uses the engine (still new and being tested). Each IE window gets its own engine window inside one shared engine process.
- For a plain IE window with no engine, start "Internet Explorer (classic)" from the GeckoIE folder in the Start menu, as said above.
- Alt+Left and Alt+Right use IE's history.
- Settings are in `ieb.ini` in the install folder: turn the engine off (`Enabled=0`), turn off starting it early (`Prewarm=0`), change its CPU priority (`Priority`), or put a soft cap on its memory (`MaxWorkingSetMB`). Changes apply the next time the engine starts.

# A VERY IMPORTANT NOTE ON AI
I want to be transparent: About 95% of this project was built using Claude AI, guided by me and my super clever instructions logic... So that means that unexpected bugs WILL happen. If u trust AI, then you might be ok with this project, If not... well, fair enough :) I am not super active now but I will work on that.

## Screenshots
Here are some early Alpha screenshots:

<p align="center">
  <img src="./images/GeckoIE_Preview.PNG" width="700">
</p>

## Known issues
 
- IE's Stop button does not stop engine page loads yet.
- Keyboard shortcuts of IE (for example Ctrl+L) do not work while the engine area has focus.
- Links that would open a new window open in the same tab.
- Only `http` and `https` addresses go to the engine. Everything else uses IE's own rendering.
- Downloads is not synced with IE, you can still download content though.
- Only the first start of the day is slow: the engine is started with IE, and on a cold, slow disk that can take 30 seconds or more.
- Each additional IE window uses extra memory but not as launching a separate engine.


## Credits and legal
 
- [Mypal](https://github.com/Feodor2/Mypal) by Feodor2, and Gecko by Mozilla, both under the Mozilla Public License 2.0.
- NSIS (Nullsoft Scriptable Install System).
- GeckoIE is an independent project. It is not affiliated with, endorsed by or sponsored by Microsoft or Mozilla. "Internet Explorer" and "Firefox" are trademarks of their owners.
- License for GeckoIE's own code: *choose one before publishing (for example MIT or MPL-2.0).*
