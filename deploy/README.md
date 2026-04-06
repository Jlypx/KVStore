# Deploy Baseline

The repository now supports two runtime styles:

- `embedded` mode, which keeps the original single-process deterministic five-node cluster
- `cluster-node` mode, where one `kvd` process represents one real node with separate client and peer listeners

For same-host development and integration testing, the repository includes:

- sample five-node static configs under `deploy/`
- local launcher helpers under `scripts/cluster/`

This is still a development baseline, not a production deployment system.
The repository does not yet ship Kubernetes, systemd, or container orchestration assets.
