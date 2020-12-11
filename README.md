[![Build Status](https://travis-ci.org/okbob/dbms_sql.svg?branch=master)](https://travis-ci.org/okbob/utl_mail)

# UTL_MAIL

This is implementation of Oracle's API of packages UTL_MAIL, DBMS_MAIL and UTL_SMTP

It doesn't ensure full compatibility, but should to decrease a work necessary for
successful migration.

## Functionality


## Dependency

This extensions uses curl library.

When you plan to use utl_mail extension together with Orafce, then you have to remove line
with `CREATE DOMAIN varchar2 AS text;` statement from install sql script.
