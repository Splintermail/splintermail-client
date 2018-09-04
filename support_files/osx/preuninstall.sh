#!/bin/sh

#
# This file is meant to be called by the Splintermail Uninstaller
#

# stop / disable the launchd service
launchctl unload -w "/Library/LaunchDaemons/com.splintermail.ditm.plist"

# get hash of original file
orig_config="QWER share_dir REWQ/splintermail.conf.orig"
if [ -f "$orig_config" ] ; then
    old_config_hash=$(shasum -a 256 "$orig_config" | awk '{print($1)}')
else
    old_config_hash="-"
fi
# if config file exists, check if it has been modified
if [ -f "QWER config_path REWQ" ] ; then
    cur_config_hash=$(shasum -a 256 QWER config_path REWQ | awk '{print($1)}')
    if [ "$old_config_hash" = "$cur_config_hash" ] ; then
        # remove the config file... it had no user info in it
        rm -f "QWER config_path REWQ"
    fi
fi

# delete the default config file (not handled by installer)
rm -f "$orig_config"

# before deleting the old CA, attempt to untrust it
if [ -f "QWER ditm_dir REWQ/QWER ca_name REWQ" ] ; then
    security remove-trusted-cert -d "QWER ditm_dir REWQ/QWER ca_name REWQ"
fi

# delete the data folder (not handled by installer)
rm -rf "QWER ditm_dir REWQ"

### not deleting service user, I agree with Fedora's guidelines
### https://fedoraproject.org/wiki/Packaging:UsersAndGroups
# # delete the _splintermail user and group
# dscl . -delete /Users/_splintermail
# dscl . -delete /Groups/_splintermail

exit 0
