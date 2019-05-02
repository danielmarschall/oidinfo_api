<?php

require_once __DIR__ . '/oidinfo_api.inc.phps';

$oa = new OIDInfoAPI();

$oa->loadIllegalityRuleFile('oid_illegality_rules');

assert($oa->illegalOID('1.3.6.1.2.1.9999') === true);
assert($oa->illegalOID('1.3.6.1.2.1.9999.123') === true);
assert($oa->illegalOID('2.999') === false);
assert($oa->illegalOID('3') === true);
assert($oa->illegalOID('1') === false);
assert($oa->illegalOID('1.0.16') === true);
assert($oa->illegalOID('1.2.6.0') === true); // 1.2.6 is illegal -> 1.2.6.0 too

assert($oa->strictCheckSyntax('0', false, true) === true);
assert($oa->strictCheckSyntax('1', false, true) === true);
assert($oa->strictCheckSyntax('(requesting)', false, true) === false);
