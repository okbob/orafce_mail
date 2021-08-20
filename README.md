[![Build Status](https://travis-ci.org/okbob/orafce_utl_mail.svg?branch=master)](https://travis-ci.org/okbob/orafce_utl_mail)

# orafce_mail

This is implementation of Oracle's API of packages utl_mail, DBMS_MAIL

It doesn't ensure full compatibility, but should to decrease a work necessary for
successful migration.

## Security

These functions can be used by user that is member of role `orafce_mail`.


## Functionality

```
set orafce_mail.smtp_server_url to 'smtps://smtp.gmail.com:465';
set orafce_mail.smtp_server_userpwd to 'pavel.stehule@gmail.com:yourgoogleapppassword';

call utl_mail.send(sender => 'pavel.stehule@gmail.com',
                   recipients => 'pavel.stehule@gmail.com',
                   subject => 'ahoj, nazdar, žlutý kůň',
                   message => e'test, \nžlutý kůň');

do $$
declare
  myimage bytea = (select img from foo limit 1);
begin
  call utl_mail.send_attach_raw(sender => 'pavel.stehule@gmail.com',
                                recipients => 'pavel.stehule@gmail.com',
                                subject => 'mail with picture',
                                message => 'I am sending some picture',
                                attachment => myimage,
                                att_mime_type => 'image/png',
                                att_filename => 'screenshot.png');
end
$$;
```

## Dependency

This extensions uses curl library.

An extension Orafce should be installed before