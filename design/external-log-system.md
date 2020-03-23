# External Log System

## Goals

1. "Tee" all committed mutations to an external data sink. This could be for
disaster recovery, live migration, ingesting into an OLAP database, auditing,
etc. In order to "tee" _all_ committed mutations, a transaction can only be
considered committed if it's acknowledged by the external log system.
2. Minimize impact on commit latency.
3. Use a protocol that's stable across FDB major versions.

## Configuration

The external log system will be configured through fdbcli. Syntax is TBD, but
the idea is that both attaching and detaching an external log system to an
fdb cluster will be done through the "configure" command. Detaching should
still work if the external log system is unavailable. Configuration is stored
in the txnStateStore as usual. Attaching and detaching will both require a recovery, as they need to rewrite the coordinated state.

## Changes to master proxy's commit protocol

Configuration changes will be considered part of the "metadata" keyspace, and
so will be propagated between proxies via the resolver as usual. This way all proxies
agree on the first commit version that must be logged on the external log
system after an attach, and the first version that need to be logged after a
detach.

Proxies will log to both the tlogs and the external log system concurrently,
and wait until both acknowledge before acknowledging a commit.

## Changes to master recovery

When ending an epoch, the master must either end the epoch on the external
log system or detach. The recovery version/epoch end of the  cluster must
not be greater than the epoch end of the external log system (to guarantee
_all_ committed mutations make it to the external log system). Thus the
external log system must be consulted before determining at what version
to read the txnStateStore.

### Attach

Changing the external log system configuration will write to the
txnStateStore and then cause a recovery. The next master will see this new
configuration after reading the txnStateStore, and will write associated
metadata to the coordinated state.

### Detach

If a recovering master sees that there's no external log system configuration
in the txnStateStore, then it will omit external log system metadata in the
coordinated state.

In order to support detaching when the external log system is unavailable, we'll start the provisional master so that it's running while we're ending the last epoch. If the provisional master gets a "detach" mutation, it will stop waiting on the external log system ending the epoch, and write the "detach" mutation in the recovery transaction.

## Changes to coordinated state

In order for the master to consult the external log system when ending the epoch, metadata for the external log system will be added to the coordinated state.
