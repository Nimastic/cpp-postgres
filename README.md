# cpp-postgres

I watched [this Hussein Nasser video](https://www.youtube.com/watch?v=q9jixKv4h2I) on how Postgres stores tables, pages, tuples, and indexes, and I wanted to actually build the thing instead of just nodding along.

This is a tiny engine in C++. It is not Postgres. It will not speak SQL for a long time. I just want to see an 8KB page on disk, stick a row in it, update that row without overwriting it, and watch an old transaction still see the old price.

The order I am building it in is in [PLAN.md](PLAN.md). One piece at a time.
