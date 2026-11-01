# Third-party code and licenses

This project incorporates the following upstream work:

| Component | Source and pinned revision | License |
| --- | --- | --- |
| Moonlight common C | https://github.com/moonlight-stream/moonlight-common-c at `b126e481a195fdc7152d211def17190e3434bcce` (fetched by CMake) | GPL-3.0-or-later; see its license files |
| GameStream control client | https://github.com/moonlight-stream/moonlight-embedded at `f32e415aea6797d261d6b470dcf8bf18727341c2`, copied into `vendor/libgamestream` | GPL-3.0-or-later |
| FBInk | https://github.com/NiLuJe/FBInk at `886f25f13368859ad8a899b88d04c26e19cda32e` (external build dependency); matching header in `vendor/fbink` | GPL-3.0-or-later |

The copied GameStream client is modified locally for pairing error propagation, fresh TLS control connections, credential consistency checks, and English diagnostic messages. The copied FBInk header has one device-list comment generalized to remove model-specific branding. Original copyright notices are retained. Nested upstream submodules retain their own licenses and pinned revisions, including ENet.

Runtime libraries, FFmpeg, GTK2, OpenSSL, curl, and the DejaVu font are separate dependencies with their own licenses. `runtime-manifest.json` and `runtime-sources.json` record the prior experimental bundle's provenance. They contain no runtime binaries. Review the applicable source and license obligations when distributing a package.

New application code and project scripts are distributed under GPL-3.0-or-later. This project is not affiliated with or endorsed by Amazon, Moonlight, Sunshine, or FBInk's authors.
