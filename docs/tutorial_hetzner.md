# Hetzner Storagebox Tutorial 

Hetzner Storageboxes support BorgBackup via SSH with user/password and SSH keys.
User/Password authentication cannot be completely disabled.
But you can pick a strong 128 char random password. 64 chars would be already sufficient.

The password is only to protect your account when opening it outside the Hetzner network for external access.
But it would be generally a good idea to protect it well also internally.


## Storagebox config

BorgBackup enabled on each storage box and can be accessed via SSH on port 23.
You can create a sub-account just for backup and upload a specific backup SSH key.

More modern version of SSH allow to push public SSH keys directly.
The Hetzner documentation provides details about SSH key upload if this method does not work.
The command creates the authorized_keys file in the right spot on your Storagebox.

```
ssh-keygen -t ed25519
ssh-copy-id -p 23 -s u123456-sub3@u123456-sub3.your-storagebox.de
```

You can test the account using he following command


```
sftp -P 23 u123456-sub3@u123456-sub3.your-storagebox.de
```


## Configure storagebox SSH session

Create or edit `/home/notes/.ssh/config` to add a new session which can be referenced by a BorgBackup session.
It is cleaner than specifying all parameter separately and can be leveraged by multipe programs.


```
vi /home/notes/.ssh/config
```

File content:


```
Host storagebox
    HostName u123456.your-storagebox.de
    User u123456-sub3
    IdentityFile /home/notes/.ssh/storagebox
    IdentitiesOnly yes
    Port 23
```


## Test connection

A first simple test would be to connect to the storagebox:

```
ssh storagebox -vv

```

## Configure Domino BorgBackup Integration


Once the configuration is verified edit the Domino Borg Backup integration.


```
vi  /etc/sysconfig/nshborg.cfg
```

File content:

```
BORG_REPO=ssh://storagebox/./domino-backup
BORG_PASSCOMMAND=cat /home/notes/.borg-pass
BORG_PASSTHRU_COMMANDS_ALLOWED=1
```

## Create a password file

The password file is the more simple integration.
The Domino Borg backup integration also can run as a credential helper with another user name to get secret information.
It would also support an SSH agent to provide the SSH key.
For thit tutorial the Hetzner integration is the main focus.


```
cat "backup4domino#" > /home/notes/.borg-pass
```

## Init repository

Because this configuration has passthru commands enabled, the following command can just use invoke the command which passes all parameters configured.
This includes the SSH configured session, the repository and the password.


```
nshborg init --encryption=keyfile-blake2
```

The repo keyfile stores the key outside the repository.
IF the remote repository would be on a secured server in your environment a repositoy key would be the easier approach.

If you use a keyfile make sure it is backed up separately.

Refer to [Borg documentation](https://borgbackup.readthedocs.io/en/stable/usage/init.html) for details.


## Check repository


Once the repository is created it can be checked with the info command.

```
nshborg info
```

When all the steps completely successfully Borg Backup is ready to be used with a matching Domino Backup configuration.
But it can be also used stand-alone if passthru commands are enabled.
Passthru commands are also helpful to look at the repository, manually extract or manage backups.


