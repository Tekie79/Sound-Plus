# M08 · Library metadata

Open **Library filters** next to search to combine fields such as `category:AIR channels:1`. Save a filter to reuse the same query, root and collection. For a category, subcategory or CatID, enter the canonical value and a search alias to save a local alias. Clear the canonical value to remove it. Open the pencil in Details to edit the selected recordings. **Use source** removes the user override for one field. The Columns menu offers embedded tags and normalized catalog fields.

SQLite schema 8 preserves asset IDs and collection references through migration. Backfill runs on the Library worker. User fields have priority over embedded and filename-derived values. The Details panel retains the raw embedded fields separately. Catalog-only bulk edits are transactional and undoable. Manifest import requires the same asset ID, revision and SHA-256. A changed source revision raises a review flag and retains user edits. Generation receipts remain bound to their source revision.

The pinned UCS 8.2.1 lookup covers 753 CatIDs. The parser recognizes an exact filename prefix and preserves unrecognized names. Its vocabulary is derived from the public UCS category list. The vendored JSON SHA-256 is `9fcfbd8f82335fd74055486cfc30a3be71d3d0e900380b3e548e34e0dd072788`. The CSV used to derive it had SHA-256 `aebc8bf4f8b0dd7cafc1231c25b6664250087ab1acb8d4e6ed18ef8bf9d47986`. [UCS](https://universalcategorysystem.com/) · [Source data](https://github.com/jmrsound/ucs-tools/blob/main/src/ucs_tools/data/ucs_v8.2.1.csv)

## Verification on Apple Silicon

Seven focused Release suites passed in 12.27 s. Tests wrote a BWF fixture, checked exact CatID and description, combined folder, favorite, metadata and semantic scope, edited and reopened an offline record, applied and undid 300 tags, and checked fingerprint conflicts and historical provider provenance. The WAV SHA-256 stayed identical. A JUCE view-command test opened the actual Details editor, applied a description, waited for the catalog-backed view and checked both marked sample boundaries. [CTest results](development.log)

A separate SQLite database held 100,000 or 500,000 asset rows with 1% matches. Each of 40 observations fetched a 100-row filtered page at a varying offset and counted FTS matches. Peak RSS is for the test process. These measurements cover catalog queries. Reindexing a physical sound library, vector-region scaling and audition under sustained interactive load belong to M09.

| Indexed assets | Median | p95 | Peak RSS |
|---:|---:|---:|---:|
| 100,000 | 8.93 ms | 12.80 ms | 98.03 MiB |
| 500,000 | 23.26 ms | 28.34 ms | 156.45 MiB |

The stage uses deterministic, user-reviewed filename vocabulary suggestions. Acoustic category inference and human classification accuracy remain unqualified. Unknown/private fields are preserved within parser limits of 512 tags and 128 KiB of values per asset. Metadata editing is catalog-only. The tests use generated files and independent catalogs. The user's running app stayed on its original bundle.

Release executable SHA-256 `0a443a5095d2502db007bf4df4e82ceaa9a52f2bf2435b632f02f332437230bf`. Test executable SHA-256 `88add8f745c52fdd7772cc977627e2ee67604a2bcca39f57ada471f0547fafa9`.
