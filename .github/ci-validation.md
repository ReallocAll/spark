# Exact-SHA validation branches

Branches under `ci/**` run the normal Build workflow on their exact head SHA. This provides an isolated way to produce Windows/Linux test artifacts for pre-merge validation without changing `develop`.
