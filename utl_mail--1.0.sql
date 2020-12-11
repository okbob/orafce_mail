/* utl_mail.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION utl_mail" to load this file. \quit
CREATE SCHEMA utl_mail;

/*
 * temp solution, at the end varchar2 from orafce will be used
 */
CREATE DOMAIN varchar2 AS text; -- should be removed, if you use Orafce

