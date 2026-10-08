# Launch checklist and announcement draft

## Before announcing

- MIT license added; preserve the license and copyright notice in distributions.
- Push the changes and verify that macOS CI passes on GitHub.
- Confirm that a fresh clone can follow the README quick start.
- Create a tagged release with tested binaries or clearly documented source builds.
- Set the repository description to: `A small C++17 key-value server for macOS: kqueue, RESP2, durable AOF, and asynchronous replication.`
- Use relevant topics: `cpp`, `cpp17`, `key-value-store`, `macos`, `kqueue`, `resp`, `database`, `systems-programming`.
- Create a few scoped issues from the contribution suggestions and label suitable ones `good first issue`.

## Announcement draft

I built VeloCache to explore the internals of a small key-value server in C++17.

It uses a native macOS kqueue event loop, supports PING/SET/GET over a subset of RESP2, persists acknowledged writes through an AOF, and replicates asynchronously to read-only nodes. Disk operations run on a dedicated worker so network PING requests can remain responsive during slow writes.

The repository includes failure tests, a native TypeScript client, and a reproducible local benchmark. It is an experimental systems project with a deliberately small command set; it does not implement the full Redis API, automatic failover, authentication, or TLS.

I would especially appreciate feedback on durability behavior, replication edge cases, and the clarity of the architecture.

https://github.com/geamnegru/VeloCache

## Sustainable follow-up

Share a technical write-up explaining one measured engineering tradeoff, such as fsync latency versus event-loop responsiveness. Publish only in communities where project sharing is welcome, with the limitations and source link. Respond to bug reports, keep examples working, and release focused improvements. Track successful first runs and useful contributions alongside stars. Avoid bought stars, unsolicited messages, and repeated promotional posts.
