#!/bin/sh

#
# This file is meant to be called by the Splintermail Installer
#

set -e

# stop / disable the launchd service
launchctl unload -w "/Library/LaunchDaemons/com.splintermail.ditm.plist" || true

exit 0
