
# oid-base.com API for PHP

## Introduction

[oid-base.com](https://www.oid-base.com/) is a public repository for Object Identifiers (OIDs). This API is available in PHP and can be used in web-interfaces (Apache module, cgi-bin, etc.) but can also be used in command line tools written in PHP (on Linux with shebang  `#!/usr/bin/php`) .

The majority of the functions provided by this API helps developers in creating XML files which can be uploaded to the OID repository to add multiple OIDs, but the API also contains other helpful utilities related to OIDs, UUIDs and the OID repository in general. The XML generation can be useful for Registration Authorities (RAs) that want to deliver their assignments to www.oid-base.com , but also for people who want to support the OID repository by adding OIDs from public sources. The XML files generated by these API functions are conform with the  [XML schema of oid-base.com](https://www.oid-base.com/oid.xsd)  and can be uploaded  [here](https://www.oid-base.com/submit.htm)  - you will also find more information about the XML submission at these pages.

An example of a XML generation code can be found here:  [oidinfo_example.phps](https://misc.daniel-marschall.de/oid-repository/api/oidinfo_example.phps)

The API is licensed under the terms of the  [Apache 2.0 License](https://www.apache.org/licenses/LICENSE-2.0)  and was written by  [Daniel Marschall](http://www.daniel-marschall.de/).

## Documentation

See the full documentation in the **index.html** file.
