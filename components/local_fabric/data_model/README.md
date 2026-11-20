# Device model

Manager commits a whole successful CHIP report before waking subscribers. The initial Descriptor report resolves logical devices and endpoint groups. Incremental reports update adapter scalars and immutable metadata in one locked commit. The model keeps native precision but notifies consumers about differences in the public cluster (properties, schemas or actions). Only changed public property values go to Device activity. Each priming snapshot starts fresh: missing values are not carried forward from an earlier subscription. If temporary JSON allocation fails, the raw report still commits and state readers are notified; that activity sample can be missed. Subscribers independently acquire their current views. A gap establishes a new baseline; there is no event replay.

Device identifiers are durable; live properties are not. Removal operates on the underlying node, including sibling logical devices. See [SCHEMA.md](SCHEMA.md) for capability and action contracts.
