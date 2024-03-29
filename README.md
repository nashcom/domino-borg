# domino-borg
Domino Borg Integration


## Introduction

This project implements a helper application to integrate Borg Backup with Domino Backup available since Domino 12.0.

Out of the box Borg Backup does not provide a functionality to add files step by step into a backup.
You can pipe the files to backup into the borg process. But the backup starts once all file names are received.

Domino Backup requires databases to be backed up after bringing each database into backup mode one by one.
A snapshot of all databases would not work, because too much delta information would need to be collected and databases would be in backup mode for a long time.

The **nshborg** helper program starts the borg process and waits for requests of files to be added to the backup.

nshborg pipes the files tar formatted into the archive (the functionality is available since Borg Backup V1.2.6).

This results in a simple flow where Domino first starts the borg process in a pre-backup script.
Then brings all databases into backup mode step by step and sends a request to nshborg to take a backup.

Finally in the post backup event, nshborg is called again to stop the backup.

The end result is a single Borg archive with all databases and delta files which might occur during backup of databases.

## Borg Backup "import-tar"

The functionality used on the Borg Backup side is [borg import-tar](https://borgbackup.readthedocs.io/en/stable/usage/tar.html).
It allows to send one or multiple tar formatted streams to running borg process.

Another important option in this context is the `--ignore-zeros` which makes it possible to send more than one tar stream.
Tar is a quite old format, which has originally developed for tap backups. In addition to the file data it also provides meta data, like user and file permissions.
nshborg leverages the existing tar binary and uses pipes between the borg process on the one side and also to the tar program for every database to backup.

## Borg Restore

The nshborg helper tool also provides a restore option.

On purpose the nshborg does not implement a prune option for security reasons. Prune operations are directly executed using the borg command.
Borg Backup provides very flexible prune operations. Domino Backup prune operations and Borg prune operations should be aligned.

## Borg Prune/Delete

Delete and prune operations are critical and should be handled with care.
nshborg supports prune and delete operations and controls the requests.

The delete operations is prevented by default and needs to be configured via `BORG_DELETE_ALLOWED=1`

Prune operations are enabled by default with a minimum of 7 days for security reasons.
A lower minimum can be configured via `BORG_MIN_PRUNE_DAYS`


## Requirements

- Borg Backup version 1.2.6 (tar support was introduced in this version. Older versions will not work!)
- borg is expected to be installed in `/usr/bin/borg`


## How to install

Compile nshborg using "**make**". The project comes with a simple makefile which also can be used to install the binary.


```make install``` compiles and installs the binary.

The binary location is `/usr/bin/nshborg`


## Domino Backup configuration

Import **nshborg.dxl** into **dominobackup.nsf**. Enable the new configuration and disable the existing configuration.
Domino Backup provides an import action for DXL based configurations, which result in a new document in the database.


## Support for encrypted repositories

nshborg also supports encrypted repositories. Borg Backup provides [multiple methods specifying a passphrase](https://borgbackup.readthedocs.io/en/stable/quickstart.html#passphrase-notes).

The most secure way is to ask another application to provide the passphrase and let the other application control the right process is asking for the passphrase.

Environment variable `BORG_PASSCOMMAND` defines the command to be executed. nshborg sets this option to the own program and provides a basic passphrase support in the first version. The password is currently hardcoded in the program.


## Configuration

Configuration location: `/etc/sysconfig/nshborg.cfg`.

Specially for containers to persist the configuration the Domino data directory is used as a fallback is `/local/notesdata/domino/nshborg.cfg`

### Borg configuration settings

The following environment BORG environment variables can be specified in nshborg.cfg and are set before invoking Borg.

See the [Borg documentation](https://borgbackup.readthedocs.io/en/stable/usage/general.html#environment-variables) for details about environment variables.

- **BORG_REPO** Borg repository
- **BORG_PASSPHRASE** Borg passphrase for repository
- **BORG_PASSCOMMAND** Borg passphrase credential helper binary
- **BORG_RSH** Borg ssh command-line
- **BORG_BASE_DIR** Borg base directory
- **BORG_REMOTE_PATH** Borg remote binary name for SSH operations


### Other configuration options

- **BORG_BINARY** location of borg binary
- **SSH_KEYFILE** SSH key file to use for SSH Agent
- **SSH_KEYLIFE** Life of key file used for SSH Agent
- **BORG_DELETE_ALLOWED** Allow delete operation (disabled by default)
- **BORG_MIN_PRUNE_DAYS** Minimum prune days (default: 7)


## Coexisting with existing backups

Domino Backup only supports one backup configuration.
If restore of older backups is required, you can move the existing database to a different file name.
This will keep the exiting backup configuration and the existing backup inventory available.

Create a new dominobackup.nsf by running the backup command once.
Import the new configuration and start with your new backup.

For restoring an existing backup, specify the configuration database explicitly using the command line option `-cfg myold-dominobackup.nsf`.


