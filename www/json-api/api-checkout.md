# JSON API: /status
([&#x2b11;JSON API Index](index.md))

# Status of Local Checkout

**Status:** implemented 20130223

**Required permissions:** n/a (local access only)

**Request:** `/json/status`

This command requires a local checkout and is analog to the "fossil
status" command.

**Request Options:** currently none.

Payload example:

```json
{
"repository":"/home/stephan/cvs/fossil/fossil.fsl",
"localRoot":"/home/stephan/cvs/fossil/fossil/",
"checkout":{
  "uuid":"b38bb4f9bd27d0347b62ecfac63c4b8f57b5c93b",
  "tags":["trunk"],
  "datetime":"2013-02-22 17:34:19 UTC",
  "timestamp":1361554459
 },
/* "parent" info is currently missing. */
"files":[
  {"name":"src/checkin.c", "status":"edited"}
  ...],
"errorCount":0 /* see notes below */
}
```

Notes:

-   The `checkout.tags` property follows the framework-wide convention
    that the first tag in the list is the primary branch and any others
    are secondary.
-   `errorCount` is +1 for each missing file. Conflicts are not treated as
    errors because the CLI 'status' command does not treat them as such.
-   TODO: Info for the parent version is currently missing.
-   TODO: "merged with" info for the checkout is currently missing.
