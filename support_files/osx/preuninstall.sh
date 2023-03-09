#!/bin/sh

#
# This file is meant to be called by the Splintermail Uninstaller
#

# stop / disable the launchd service
launchctl unload -w "/Library/LaunchDaemons/com.splintermail.citm.plist" || true

# get hash of original file
orig_config="QW share_dir WQ/splintermail.conf.orig"
if [ -f "$orig_config" ] ; then
    old_config_hash=$(shasum -a 256 "$orig_config" | awk '{print($1)}')
else
    old_config_hash="-"
fi
# if config file exists, check if it has been modified
if [ -f "QW config_path WQ" ] ; then
    cur_config_hash=$(shasum -a 256 QW config_path WQ | awk '{print($1)}')
    if [ "$old_config_hash" = "$cur_config_hash" ] ; then
        # remove the config file... it had no user info in it
        rm -f "QW config_path WQ"
    fi
fi

# delete the default config file (not handled by installer)
rm -f "$orig_config"

# before attempt to untrust/delete the old certificate it
security delete-certificate -c "QW ca_common_name WQ" || true

# delete the data folder (not handled by installer)
rm -rf "QW sm_dir WQ"

### not deleting service user, I agree with Fedora's guidelines
### https://fedoraproject.org/wiki/Packaging:UsersAndGroups
# # delete the _splintermail user and group
# dscl . -delete /Users/_splintermail
# dscl . -delete /Groups/_splintermail

exit 0
