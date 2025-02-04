
# vim:set ft= ts=4 sw=4 et fdm=marker:

use Test::Nginx::Socket "no_plan";

$ENV{'TEST_NGINX_PHP_VERSION'} = $ENV{'PHP_SRC_VERSION'};

run_tests();

__DATA__
=== TEST 1: ini file
ini file
--- http_config
php_ini_path /etc/php/$TEST_NGINX_PHP_VERSION/embed/php.ini;
--- config
location = /ini {
    content_by_php '
        echo php_ini_loaded_file();
    ';
}
--- request
GET /ini
--- response_body eval
"/etc/php/" . $ENV{'PHP_SRC_VERSION'} . "/embed/php.ini"