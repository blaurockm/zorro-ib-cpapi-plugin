# Interactive Brokers Client Portal API Plugin for Zorro-Project

## Using the plugin

1. Download the client portal gateway.
2. 

### Authentication

Interactive Brokers requires an authentication once a day. 
Automatic (re-)authentication with reduced security of you account can be achieved with https://github.com/Voyz/ibeam

### Trading limitations

you have on Session for all active Zorro-Instances.
All your scripts shared the limitation of 10 market-data-requests per second. Meaning can cannot have more
than 5 assets with minute-bar-data.


## Compiling the plugin

for now you have to use Visual Studio, community edition is ok.
You have to have a Zorro installation because of the header files and ZorroDLL.cpp


## Links

* [Zorro Project](www.zorro-project.org)
* [IB-Plugin](https://www.interactivebrokers.com/en/trading/ib-api.php)
* 

