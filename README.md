# UHK Firmware with Extended macro engine

This fork of UHK firmware extends the keyboard with a "simple" macro language that allows users to customize behaviour of the UHK from user space.

These include things like:
- macro commands to switch keymaps (`switchKeymap`) or layers (`holdLayer`, `toggleLayer`), activate keys (`holdKey`, `tapKey`),
- some basic conditionals (e.g., `ifCtrl`, `ifDoubletap`), allowing to mimic secondary roles and various timeout scenarios
- super-simple control flow (basically `goTo` instruction), and commands to activate other macros (`call`, `fork`, `exec`)
- many configuration options which are not directly exposed by Agent (`set`)
- runtime macro recorder implemented on scancode level, for vim-like macro functionality

If you want to give it a try, you should continue at.

- [Getting started](doc-dev/user-guide.md)
- [Reference manual](doc-dev/reference-manual.md)

# Other notes

## Migration

Since 9.0.0, there are some groundbreaking changes:

- This branch has been fully merged into uhk/master, and further improvements are going to be merged straight into uhk/master.

- There is now a dedicated command macro action.

- Multiline scripts are now supported.

From now on, please use the official firmware.

In order to migrate to official build:

- You need export your UserConfig.json, transform all relevant text actions to command actions and import your UserConfig.json back. You can use IzK666's conversion tool which is available at [https://izk666.github.io/UHK-Viewer-v5/](https://izk666.github.io/UHK-Viewer-v5/), or use any automation tool of your choice (vim macro, sed, ...) to transform the file into the required format.

   From 

   ```
   "macroActionType": "text",
   "text": "printStatus"
   ```

   To

   ```
   "macroActionType": "command",
   "command": "printStatus"
   ```

- Some macro commands have been renamed, deprecated or even removed during past two years. You may need to look these up in the docs and transform to the new form. This especially applies to commands of form `setSomething`, which have become `set something` instead.

## Contributing

If you wish some functionality, feel free to fire tickets with feature requests. If you wish something already present on the tracker (e.g., in 'idea' tickets), say so in comments. (Feel totally free to harass me over desired functionality :-).) If you feel brave, fork the repo, implement the desired functionality and post a PR.

If you decide to implement new functionality, please do so on top of `common_trunk_upstream`.

## Branches
 
Currently, there are following important branches:
 
 - `common_trunk` is branch that contains things that are common (or are intended to become common) between this fork and stock firmware.  
 - `common_trunk_upstream` is similar to `common_trunk`, but merges PRs (built on top of `common_trunk`) before they get accepted into stock firmware. This is my working development version.
 - `master` - merges from `common_trunk`/`common_trunk_upstream`, but contains things specific to this fork. 
 - `uhk_master` - tracking branch of uhk/master which additionally contains true `common_trunk` merges. All accepted PRs from `common_trunk` into `uhk/master` are furthermore merged into `uhk_master` from both `common_trunk` *and* `uhk_master`. Consequently, `uhk_master` is merged into `common_trunk` and `common_trunk_upstream`. This is prevents merge conflicts (since stock firmware often merges by a squash commit, which only creates a new rebased commit without creating any link to the merged history.)
 

## Adding new features

The key file is `usb_report_updater.c` and its `UpdateUsbReports` function. All keyboard logic is driven from here.

Our command actions are rooted in `processCommandAction(...)` in `macros.c`.

If you have any questions regarding the code, simply ask (via tickets or email).

## Building the firmware

If you want to try the firmware out, just download the tar in releases and flash it via Agent.

If you wish to make changes into the source code, then you indeed need to build your own firmware:

- Clone the repo with `--recursive` flag.
- Build agent in lib/agent (that is, unmodified official agent), via `npm install && npm run build` in repository root. While doing so, you may run into some problems:
  - You may need to remove `node_modules` directory for number of unintuitive reasons. E.g., if things just stopped working out of nothing.
  - You may need to run `npm install` and maybe `npm run build` in various directories - in such cases, it is usually noted in their README.md
  - You may need to install some packages globally.
  - You may need to downgrade or upgrade npm: `sudo npm install -g n && sudo n 8.12.0`
  - You may need to commit changes made by npm in this repo, otherwise, make-release.js will be faililng later.
  - You may need to offer some sacrifice the node.js gods.
- Then you can setup mcuxpressoide according to the official firmware README guide. (Optionaly - any C-capable editor of choice will work just fine.)
- Now you can build and flash firmware either: (No special equipment is needed.)
  - `make` / `make flash` in `right/uhk60v1`.
  - Or via mcuxpressoide (debugging probes are not needed, see official firmware README).
  - Or via running scripts/make-release.js and flashing the resulting tar through agent.

If you have any problems with the build procedure (especially witn npm), please create issue in the official agent repository. I made no changes into the proccedure and I will most likely not be able to help.

## Philosophy

- Functionality of the firmware should consist of small but generic building blocks which can be combined to support new usecases.

- The engine is designed to be super simple and have very small memory footprint.

- The aim of the command interface is mostly to provide direct interface between the user and experimental new features in the firmware. (That is, to bypass the fuss of having to implement new user interface, extending two parses, three different config formats, and ensuring compatibility of it all.) My goal never was to provide a strong scripting language. In this light, I hope that lack of scoping, reasonable control flow, named variables or shunting yard expressions is somewhat understandable.




