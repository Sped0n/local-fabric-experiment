# Device model

Manager commits a whole successful CHIP report before waking subscribers. The initial Descriptor report resolves logical devices and endpoint groups. Incremental reports update adapter scalars and immutable metadata in one locked commit. Subscribers independently acquire their current views. A gap establishes a new baseline; there is no event replay.

Device identifiers are durable; live properties are not. Removal operates on the underlying node, including sibling logical devices. See [SCHEMA.md](SCHEMA.md) for capability and action contracts.
