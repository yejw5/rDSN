<?php
require_once($argv[1]); // type.php
require_once($argv[2]); // program.php
$file_prefix = $argv[3];
?>

using global::System;
using global::System.Collections.Generic;
using rDSN.Tron.Contract;

namespace <?=$_PROG->get_csharp_namespace()?> 
{
    [TronService]
    <?php foreach ($_PROG->services as $svc) { ?>
    public interface <?=$svc->name?> {
        <?php foreach ($svc->functions as $f) { ?>
        <?=$f->get_csharp_return_type()?> <?=$f->name?>(<?=$f->get_first_param()->get_csharp_type()?> <?=$f->get_first_param()->name?>);
        <?php } ?>
    }
    <?php } ?>


    <?php foreach ($_PROG->structs as $s) { ?>
    public class <?=$s->get_csharp_name()?>
    {   
        <?php foreach ($s->fields as $fld) { ?>
        public <?=$fld->get_csharp_type()?> <?=$fld->name?>;
        <?php } ?>
    }
    <?php } ?>

    <?php foreach ($_PROG->enums as $em) { ?>
    public enum <?=$em->get_csharp_name()?> {
        <?php foreach ($em->values as $k => $v) { ?>
        <?=$k?> = <?=$v?>,
        <?php } ?>
    }
    <?php } ?>
}